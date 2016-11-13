#include "ClipperUtils.hpp"
#include "ExtrusionEntityCollection.hpp"
#include "PerimeterGenerator.hpp"
#include "Print.hpp"
#include "Layer.hpp"
#include "SupportMaterial.hpp"
#include "Fill/FillBase.hpp"
#include "SVG.hpp"

#include <cmath>
#include <cassert>
#include <memory>

#define SLIC3R_DEBUG

namespace Slic3r {

// Increment used to reach MARGIN in steps to avoid trespassing thin objects
#define NUM_MARGIN_STEPS 3

// Dimensions of a tree-like structure to save material
#define PILLAR_SIZE (2.5)
#define PILLAR_SPACING 10

PrintObjectSupportMaterial::PrintObjectSupportMaterial(const PrintObject *object) :
    m_object                (object),
    m_print_config          (&object->print()->config),
    m_object_config         (&object->config),

    m_first_layer_flow (Flow::new_from_config_width(
        frSupportMaterial,
        (object->print()->config.first_layer_extrusion_width.value > 0) ? object->print()->config.first_layer_extrusion_width : object->config.support_material_extrusion_width,
        object->print()->config.nozzle_diameter.get_at(object->config.support_material_extruder-1),
        object->config.get_abs_value("first_layer_height"),
        false
    )),
    m_support_material_flow (Flow::new_from_config_width(
        frSupportMaterial, 
        (object->config.support_material_extrusion_width.value > 0) ? object->config.support_material_extrusion_width : object->config.extrusion_width,
        object->print()->config.nozzle_diameter.get_at(object->config.support_material_extruder-1),
        object->config.layer_height.value,
        false)), 
    m_support_material_interface_flow(Flow::new_from_config_width(
        frSupportMaterialInterface,
        (object->config.support_material_extrusion_width.value > 0) ? object->config.support_material_extrusion_width : object->config.extrusion_width,
        object->print()->config.nozzle_diameter.get_at(object->config.support_material_interface_extruder-1),
        object->config.layer_height.value,
        false)),
    m_soluble_interface (object->config.support_material_contact_distance.value == 0),
 
    m_support_material_raft_base_flow(0, 0, 0, false),
    m_support_material_raft_interface_flow(0, 0, 0, false),
    m_support_material_raft_contact_flow(0, 0, 0, false),

    m_has_raft                  (object->config.raft_layers.value > 0),
    m_num_base_raft_layers      (0),
    m_num_interface_raft_layers (0),
    m_num_contact_raft_layers   (0),

    // If set, the raft contact layer is laid with round strings, which are easily detachable
    // from both the below and above layes.
    // Otherwise a normal flow is used and the strings are squashed against the layer below, 
    // creating a firm bond with the layer below and making the interface top surface flat.
#if 1
    // This is the standard Slic3r behavior.
    m_raft_contact_layer_bridging(false),
    m_object_1st_layer_bridging (true),
#else
    // This is more akin to what Simplify3D or Zortrax do.
    m_raft_contact_layer_bridging(true),
    m_object_1st_layer_bridging (false),
#endif

    m_raft_height               (0.),
    m_raft_base_height          (0.),
    m_raft_interface_height     (0.),
    m_raft_contact_height       (0.),

    // 50 mirons layer
    m_support_layer_height_min  (0.05),
    m_support_layer_height_max  (0.),
    m_support_interface_layer_height_max(0.),

    m_gap_extra_above           (0.2),
    m_gap_extra_below           (0.2),
    m_gap_xy                    (0.2),

    // If enabled, the support layers will be synchronized with object layers.
    // This does not prevent the support layers to be combined.
    m_synchronize_support_layers_with_object(false),
    // If disabled and m_synchronize_support_layers_with_object,
    // the support layers will be synchronized with the object layers exactly, no layer will be combined.
    m_combine_support_layers    (true)
{
    // Based on the raft style and size, initialize the raft layers and the 1st object layer attributes.

    size_t num_raft_layers = m_object_config->raft_layers.value;

    //FIXME better to draw thin strings, which are easier to remove from the object.
    if (m_has_raft) 
    {
        if (m_raft_contact_layer_bridging)
        m_support_material_raft_contact_flow = Flow::new_from_spacing(
                m_support_material_raft_interface_flow.spacing(), 
                m_support_material_raft_interface_flow.nozzle_diameter, 
                m_support_material_raft_interface_flow.height, 
                true);

        if (m_raft_contact_layer_bridging && num_raft_layers == 1)
            // The bridging contact layer will not bond to the bed well on its own.
            // Ensure there is at least the 1st layer printed with a firm squash.
            ++ num_raft_layers;

        // Split the raft layers into a single contact layer
        // and an equal number of interface and base layers,
        // with m_num_interface_raft_layers >= m_num_base_raft_layers.
        m_num_contact_raft_layers = 1;
        m_num_interface_raft_layers = num_raft_layers / 2;
        m_num_base_raft_layers = num_raft_layers - m_num_contact_raft_layers - m_num_interface_raft_layers;
        assert(m_num_interface_raft_layers >= m_num_base_raft_layers);
        assert(m_num_contact_raft_layers + m_num_base_raft_layers + m_num_interface_raft_layers == num_raft_layers);

        m_raft_contact_height = m_num_contact_raft_layers * m_support_material_raft_contact_flow.height;
        if (m_num_base_raft_layers > 0) {
            m_raft_base_height = first_layer_height() + (m_num_base_raft_layers - 1) * m_support_material_raft_base_flow.height;
            m_raft_interface_height = m_num_interface_raft_layers * m_support_material_raft_interface_flow.height;
        } else if (m_num_interface_raft_layers > 0) {
            m_raft_base_height = 0;
            m_raft_interface_height = first_layer_height() + (m_num_interface_raft_layers - 1) * m_support_material_raft_interface_flow.height;
        } else {
            m_raft_base_height = 0;
            m_raft_interface_height = 0;
        }
        m_raft_height = m_raft_base_height + m_raft_interface_height + m_raft_contact_height;

        // Find the layer height of the 1st object layer.
        if (m_object_1st_layer_bridging) {
            // Use an average nozzle diameter.
            std::set<size_t> extruders = m_object->print()->object_extruders();
            coordf_t nozzle_dmr = 0;
            for (std::set<size_t>::const_iterator it = extruders.begin(); it != extruders.end(); ++ it) {
                nozzle_dmr += m_object->print()->config.nozzle_diameter.get_at(*it);
            }
            nozzle_dmr /= extruders.size();
            m_object_1st_layer_height = nozzle_dmr;
        } else {
            m_object_1st_layer_height = m_object->config.layer_height.value;
            for (t_layer_height_ranges::const_iterator it = m_object->layer_height_ranges.begin(); it != m_object->layer_height_ranges.end(); ++ it) {
                if (m_object_1st_layer_height >= it->first.first && m_object_1st_layer_height <= it->first.second) {
                    m_object_1st_layer_height = it->second;
                    break;
                }
            }
        }

        m_object_1st_layer_gap = m_soluble_interface ? 0. : m_object_config->support_material_contact_distance.value;
        m_object_1st_layer_print_z = m_raft_height + m_object_1st_layer_gap + m_object_1st_layer_height;
    }
    else
    {
        // No raft.
        m_raft_contact_layer_bridging = false;
        m_object_1st_layer_bridging  = false;
        m_object_1st_layer_height    = m_first_layer_flow.height;
        m_object_1st_layer_gap       = 0;
        m_object_1st_layer_print_z   = m_object_1st_layer_height;
    }
}

// Using the std::deque as an allocator.
inline PrintObjectSupportMaterial::MyLayer& layer_allocate(
    std::deque<PrintObjectSupportMaterial::MyLayer> &layer_storage, 
    PrintObjectSupportMaterial::SupporLayerType      layer_type)
{ 
    layer_storage.push_back(PrintObjectSupportMaterial::MyLayer());
    layer_storage.back().layer_type = layer_type;
    return layer_storage.back();
}

inline void layers_append(PrintObjectSupportMaterial::MyLayersPtr &dst, const PrintObjectSupportMaterial::MyLayersPtr &src)
{
    dst.insert(dst.end(), src.begin(), src.end());
}

// Compare layers lexicographically.
struct MyLayersPtrCompare
{
    bool operator()(const PrintObjectSupportMaterial::MyLayer* layer1, const PrintObjectSupportMaterial::MyLayer* layer2) const {
        return *layer1 < *layer2;
    }
};

void PrintObjectSupportMaterial::generate(PrintObject &object)
{
    coordf_t max_object_layer_height = 0.;
    for (size_t i = 0; i < object.layer_count(); ++ i)
        max_object_layer_height = std::max(max_object_layer_height, object.get_layer(i)->height);

    if (m_support_layer_height_max == 0)
        m_support_layer_height_max = std::max(max_object_layer_height, 0.75 * m_support_material_flow.nozzle_diameter);
    if (m_support_interface_layer_height_max == 0)
        m_support_interface_layer_height_max = std::max(max_object_layer_height, 0.75 * m_support_material_interface_flow.nozzle_diameter);

    // Layer instances will be allocated by std::deque and they will be kept until the end of this function call.
    // The layers will be referenced by various LayersPtr (of type std::vector<Layer*>)
    MyLayerStorage layer_storage;

    // Determine the top contact surfaces of the support, defined as:
    // contact = overhangs - clearance + margin
    // This method is responsible for identifying what contact surfaces
    // should the support material expose to the object in order to guarantee
    // that it will be effective, regardless of how it's built below.
    // If raft is to be generated, the 1st top_contact layer will contain the 1st object layer silhouette without holes.
    MyLayersPtr top_contacts = this->top_contact_layers(object, layer_storage);
    if (top_contacts.empty())
        // Nothing is supported, no supports are generated.
        return;

#ifdef SLIC3R_DEBUG
    static int iRun = 0;
    iRun ++;
    for (MyLayersPtr::const_iterator it = top_contacts.begin(); it != top_contacts.end(); ++ it) {
        const MyLayer &layer = *(*it);
        ::Slic3r::SVG svg(debug_out_path("support-top-contacts-%d-%lf.svg", iRun, layer.print_z), get_extents(layer.polygons));
        Slic3r::ExPolygons expolys = union_ex(layer.polygons, false);
        svg.draw(expolys);
    }
#endif /* SLIC3R_DEBUG */

    // Determine the bottom contact surfaces of the supports over the top surfaces of the object.
    // Depending on whether the support is soluble or not, the contact layer thickness is decided.
    MyLayersPtr bottom_contacts = this->bottom_contact_layers(object, top_contacts, layer_storage);

#ifdef SLIC3R_DEBUG
    for (MyLayersPtr::const_iterator it = bottom_contacts.begin(); it != bottom_contacts.end(); ++ it) {
        const MyLayer &layer = *(*it);
        ::Slic3r::SVG svg(debug_out_path("support-bottom-contacts-%d-%lf.svg", iRun, layer.print_z), get_extents(layer.polygons));
        Slic3r::ExPolygons expolys = union_ex(layer.polygons, false);
        svg.draw(expolys);
    }
#endif /* SLIC3R_DEBUG */

    // Because the top and bottom contacts are thick slabs, they may overlap causing over extrusion 
    // and unwanted strong bonds to the object.
    // Rather trim the top contacts by their overlapping bottom contacts to leave a gap instead of over extruding.
    this->trim_top_contacts_by_bottom_contacts(object, bottom_contacts, top_contacts);

    // Generate empty intermediate layers between the top / bottom support contact layers,
    // The layers may or may not be synchronized with the object layers, depending on the configuration.
    // For example, a single nozzle multi material printing will need to generate a waste tower, which in turn
    // wastes less material, if there are as little layers as possible, therefore minimizing the material swaps.
    MyLayersPtr intermediate_layers = this->raft_and_intermediate_support_layers(
        object, bottom_contacts, top_contacts, layer_storage, max_object_layer_height);

    // Fill in intermediate layers between the top / bottom support contact layers, trimmed by the object.
    this->generate_base_layers(object, bottom_contacts, top_contacts, intermediate_layers);

#ifdef SLIC3R_DEBUG
    for (MyLayersPtr::const_iterator it = intermediate_layers.begin(); it != intermediate_layers.end(); ++ it) {
        const MyLayer &layer = *(*it);
        ::Slic3r::SVG svg(debug_out_path("support-base-layers-%d-%lf.svg", iRun, layer.print_z), get_extents(layer.polygons));
        Slic3r::ExPolygons expolys = union_ex(layer.polygons, false);
        svg.draw(expolys);
    }
#endif /* SLIC3R_DEBUG */

    // If raft is to be generated, the 1st top_contact layer will contain the 1st object layer silhouette without holes.
    // Add the bottom contacts to the raft, inflate the support bases.
    // There is a contact layer below the 1st object layer in the bottom contacts.
    // There is also a 1st intermediate layer containing bases of support columns.
    // Extend the bases of the support columns and create the raft base.
    Polygons raft = this->generate_raft_base(object, bottom_contacts, intermediate_layers);

/*
    // If we wanted to apply some special logic to the first support layers lying on
    // object's top surfaces this is the place to detect them
    LayersSet shape;
    if (m_objectconfig->support_material_pattern.value == smpPillars)
        shape = this->generate_pillars_shape(contact, support_z);
*/

    // Propagate top / bottom contact layers to generate interface layers.
    MyLayersPtr interface_layers = this->generate_interface_layers(
        object, bottom_contacts, top_contacts, intermediate_layers, layer_storage);

#ifdef SLIC3R_DEBUG
    for (MyLayersPtr::const_iterator it = interface_layers.begin(); it != interface_layers.end(); ++ it) {
        const MyLayer &layer = *(*it);
        ::Slic3r::SVG svg(debug_out_path("support-interface-layers-%d-%lf.svg", iRun, layer.print_z), get_extents(layer.polygons));
        Slic3r::ExPolygons expolys = union_ex(layer.polygons, false);
        svg.draw(expolys);
    }
#endif /* SLIC3R_DEBUG */

/*
    // Clip with the pillars.
    if (! shape.empty()) {
        this->clip_with_shape(interface, shape);
        this->clip_with_shape(base, shape);
    }
*/

    // Install support layers into the object.
    MyLayersPtr layers_sorted;
    layers_sorted.reserve(bottom_contacts.size() + top_contacts.size() + intermediate_layers.size() + interface_layers.size());
    layers_append(layers_sorted, bottom_contacts);
    layers_append(layers_sorted, top_contacts);
    layers_append(layers_sorted, intermediate_layers);
    layers_append(layers_sorted, interface_layers);
    std::sort(layers_sorted.begin(), layers_sorted.end(), MyLayersPtrCompare());

    int layer_id = 0;
    for (int i = 0; i < int(layers_sorted.size());) {
        // Find the last layer with the same print_z, find the minimum layer height of all.
        int j = i + 1;
        coordf_t height_min = layers_sorted[i]->height;
        for (; j < layers_sorted.size() && layers_sorted[i]->print_z == layers_sorted[j]->print_z; ++ j) 
            height_min = std::min(height_min, layers_sorted[j]->height);
        object.add_support_layer(layer_id, height_min, layers_sorted[i]->print_z);
        if (layer_id > 0) {
            SupportLayer *sl1 = object.support_layers[object.support_layer_count()-2];
            SupportLayer *sl2 = object.support_layers.back();
            sl1->upper_layer = sl2;
            sl2->lower_layer = sl1;
        }
        i = j;
        ++ layer_id;
    }
    
    // Generate the actual toolpaths and save them into each layer.
    this->generate_toolpaths(object, raft, bottom_contacts, top_contacts, intermediate_layers, interface_layers);
}

void collect_region_slices_by_type(const Layer &layer, SurfaceType surface_type, Polygons &out)
{
    // 1) Count the new polygons first.
    size_t n_polygons_new = 0;
    for (LayerRegionPtrs::const_iterator it_region = layer.regions.begin(); it_region != layer.regions.end(); ++ it_region) {
        const LayerRegion       &region = *(*it_region);
        const SurfaceCollection &slices = region.slices;
        for (Surfaces::const_iterator it = slices.surfaces.begin(); it != slices.surfaces.end(); ++ it) {
            const Surface &surface = *it;
            if (surface.surface_type == surface_type)
                n_polygons_new += surface.expolygon.holes.size() + 1;
        }
    }

    // 2) Collect the new polygons.
    out.reserve(out.size() + n_polygons_new);
    for (LayerRegionPtrs::const_iterator it_region = layer.regions.begin(); it_region != layer.regions.end(); ++ it_region) {
        const LayerRegion       &region = *(*it_region);
        const SurfaceCollection &slices = region.slices;
        for (Surfaces::const_iterator it = slices.surfaces.begin(); it != slices.surfaces.end(); ++ it) {
            const Surface &surface = *it;
            if (surface.surface_type == surface_type)
                polygons_append(out, surface.expolygon);
        }
    }
}

Polygons collect_region_slices_by_type(const Layer &layer, SurfaceType surface_type)
{
    Polygons out;
    collect_region_slices_by_type(layer, surface_type, out);
    return out;
}

// Collect outer contours of all expolygons in all layer region slices.
void collect_region_slices_outer(const Layer &layer, Polygons &out)
{
    // 1) Count the new polygons first.
    size_t n_polygons_new = 0;
    for (LayerRegionPtrs::const_iterator it_region = layer.regions.begin(); it_region != layer.regions.end(); ++ it_region) {
        const LayerRegion &region = *(*it_region);
        n_polygons_new += region.slices.surfaces.size();
    }

    // 2) Collect the new polygons.
    out.reserve(out.size() + n_polygons_new);
    for (LayerRegionPtrs::const_iterator it_region = layer.regions.begin(); it_region != layer.regions.end(); ++ it_region) {
        const LayerRegion &region = *(*it_region);
        for (Surfaces::const_iterator it = region.slices.surfaces.begin(); it != region.slices.surfaces.end(); ++ it)
            out.push_back(it->expolygon.contour);
    }
}

// Collect outer contours of all expolygons in all layer region slices.
Polygons collect_region_slices_outer(const Layer &layer)
{
    Polygons out;
    collect_region_slices_outer(layer, out);
    return out;
}

// Collect outer contours of all expolygons in all layer region slices.
void collect_slices_outer(const Layer &layer, Polygons &out)
{
    out.reserve(out.size() + layer.slices.expolygons.size());
    for (ExPolygons::const_iterator it = layer.slices.expolygons.begin(); it != layer.slices.expolygons.end(); ++ it)
        out.push_back(it->contour);
}

// Collect outer contours of all expolygons in all layer region slices.
Polygons collect_slices_outer(const Layer &layer)
{
    Polygons out;
    collect_slices_outer(layer, out);
    return out;
}

// Find the top contact surfaces of the support or the raft.
PrintObjectSupportMaterial::MyLayersPtr PrintObjectSupportMaterial::top_contact_layers(const PrintObject &object, MyLayerStorage &layer_storage) const
{
#ifdef SLIC3R_DEBUG
    static int iRun = 0;
    ++ iRun; 
#endif /* SLIC3R_DEBUG */

    // Output layers, sorte by top Z.
    MyLayersPtr contact_out;

    // If user specified a custom angle threshold, convert it to radians.
    double threshold_rad = 0.;
    if (m_object_config->support_material_threshold.value > 0) {
        threshold_rad = M_PI * double(m_object_config->support_material_threshold.value + 1) / 180.; // +1 makes the threshold inclusive
        // Slic3r::debugf "Threshold angle = %d°\n", rad2deg($threshold_rad);
    }
    
    // Build support on a build plate only? If so, then collect top surfaces into $buildplate_only_top_surfaces
    // and subtract $buildplate_only_top_surfaces from the contact surfaces, so
    // there is no contact surface supported by a top surface.
    bool buildplate_only = m_object_config->support_material.value && m_object_config->support_material_buildplate_only.value;
    Polygons buildplate_only_top_surfaces;

    // Determine top contact areas.
    for (size_t layer_id = 0; layer_id < object.layer_count(); ++ layer_id) {
        // Note that layer_id < layer->id when raft_layers > 0 as the layer->id incorporates the raft layers.
        // So layer_id == 0 means first object layer and layer->id == 0 means first print layer if there are no explicit raft layers.
        if (this->has_raft()) {
            if (! this->has_support() && layer_id > 0)
                // If we are only going to generate raft. Just check for the 'overhangs' of the first object layer.
                break;
            // Check for the overhangs at any object layer including the 1st layer.
        } else if (layer_id == 0) {
            // No raft, 1st object layer cannot be supported by a support contact layer as it sticks directly to print bed.
            continue;
        }

        const Layer &layer = *object.get_layer(layer_id);

        if (buildplate_only) {
            // Collect the top surfaces up to this layer and merge them.
            Polygons projection_new = collect_region_slices_by_type(layer, stTop);
            if (! projection_new.empty()) {
                // Merge the new top surfaces with the preceding top surfaces.
                // Apply the safety offset to the newly added polygons, so they will connect
                // with the polygons collected before,
                // but don't apply the safety offset during the union operation as it would
                // inflate the polygons over and over.
                projection_new = offset(projection_new, scale_(0.01));
                polygons_append(buildplate_only_top_surfaces, projection_new);
                buildplate_only_top_surfaces = union_(buildplate_only_top_surfaces, false); // don't apply the safety offset.
            }
        }

        // Detect overhangs and contact areas needed to support them.
        Polygons overhang_polygons;
        Polygons contact_polygons;
        if (layer_id == 0) {
            // This is the first object layer, so the object is being printed on a raft and
            // we're here just to get the object footprint for the raft.
            // We only consider contours and discard holes to get a more continuous raft.
            overhang_polygons = collect_slices_outer(layer);
            // Extend by SUPPORT_MATERIAL_MARGIN, which is 1.5mm
            contact_polygons = offset(overhang_polygons, scale_(SUPPORT_MATERIAL_MARGIN));
        } else {
            // Generate overhang / contact_polygons for non-raft layers.
            const Layer &lower_layer = *object.get_layer(int(layer_id)-1);
            for (LayerRegionPtrs::const_iterator it_layerm = layer.regions.begin(); it_layerm != layer.regions.end(); ++ it_layerm) {
                const LayerRegion &layerm = *(*it_layerm);
                // Extrusion width accounts for the roundings of the extrudates.
                // It is the maximum widh of the extrudate.
                coord_t fw = layerm.flow(frExternalPerimeter).scaled_width();
                coordf_t lower_layer_offset = 
                    (layer_id < m_object_config->support_material_enforce_layers.value) ? 
                        // Enforce a full possible support, ignore the overhang angle.
                        0 :
                    (threshold_rad > 0. ? 
                        // Overhang defined by an angle.
                        scale_(lower_layer.height * cos(threshold_rad) / sin(threshold_rad)) :
                        // Overhang defined by half the extrusion width.
                        0.5 * fw);
                // Overhang polygons for this layer and region.
                Polygons diff_polygons;
                if (lower_layer_offset == 0.) {
                    // Support everything.
                    diff_polygons = diff(
                        (Polygons)layerm.slices,
                        (Polygons)lower_layer.slices);
                } else {
                    // Get the regions needing a suport.
                    diff_polygons = diff(
                        (Polygons)layerm.slices,
                        offset((Polygons)lower_layer.slices, lower_layer_offset));
                    // Collapse very tiny spots.
                    diff_polygons = offset2(diff_polygons, -0.1*fw, +0.1*fw);
                    if (diff_polygons.empty())
                        continue;
                    // Offset the support regions back to a full overhang, restrict them to the full overhang.
                    diff_polygons = diff(intersection(offset(diff_polygons, lower_layer_offset), (Polygons)layerm.slices), (Polygons)lower_layer.slices);
                }
                if (diff_polygons.empty())
                    continue;

                #ifdef SLIC3R_DEBUG
                {
                    ::Slic3r::SVG svg(debug_out_path("support-top-contacts-raw-run%d-layer%d-region%d.svg", iRun, layer_id, it_layerm - layer.regions.begin()), get_extents(diff_polygons));
                    Slic3r::ExPolygons expolys = union_ex(diff_polygons, false);
                    svg.draw(expolys);
                }
                #endif /* SLIC3R_DEBUG */

                if (m_object_config->dont_support_bridges) {
                    // compute the area of bridging perimeters
                    // Note: this is duplicate code from GCode.pm, we need to refactor
                    
                    Polygons bridged_perimeters;
                    {
                        Flow bridge_flow = layerm.flow(frPerimeter, true);
                        
                        coordf_t nozzle_diameter = m_print_config->nozzle_diameter.get_at(
                            layerm.region()->config.perimeter_extruder-1);
                        Polygons lower_grown_slices = offset((Polygons)lower_layer.slices, 0.5f*scale_(nozzle_diameter));
                        
                        // TODO: split_at_first_point() could split a bridge mid-way
                        Polylines overhang_perimeters;
                        for (ExtrusionEntitiesPtr::const_iterator it_island = layerm.perimeters.entities.begin(); it_island != layerm.perimeters.entities.end(); ++ it_island) {
                            const ExtrusionEntityCollection *island = dynamic_cast<ExtrusionEntityCollection*>(*it_island);
                            assert(island != NULL);
                            for (size_t i = 0; i < island->entities.size(); ++ i) {
                                ExtrusionEntity *entity = island->entities[i];
                                ExtrusionLoop *loop = dynamic_cast<Slic3r::ExtrusionLoop*>(entity);
                                overhang_perimeters.push_back(loop ? 
                                    loop->as_polyline() :
                                    dynamic_cast<const Slic3r::ExtrusionPath*>(entity)->polyline);
                            }
                        }
                        
                        // workaround for Clipper bug, see Slic3r::Polygon::clip_as_polyline()
                        for (Polylines::iterator it = overhang_perimeters.begin(); it != overhang_perimeters.end(); ++ it)
                            it->points[0].x += 1;
                        diff(overhang_perimeters, lower_grown_slices, &overhang_perimeters);
                        
                        // only consider straight overhangs
                        // only consider overhangs having endpoints inside layer's slices
                        // convert bridging polylines into polygons by inflating them with their thickness
                        // since we're dealing with bridges, we can't assume width is larger than spacing,
                        // so we take the largest value and also apply safety offset to be ensure no gaps
                        // are left in between
                        coordf_t w = std::max(bridge_flow.scaled_width(), bridge_flow.scaled_spacing());
                        for (Polylines::iterator it = overhang_perimeters.begin(); it != overhang_perimeters.end(); ++ it) {
                            if (it->is_straight()) {
                                it->extend_start(fw);
                                it->extend_end(fw);
                                if (layer.slices.contains(it->first_point()) && layer.slices.contains(it->last_point())) {
                                    // Offset a polyline into a polygon.
                                    Polylines tmp; tmp.push_back(*it);
                                    Polygons out;
                                    offset(tmp, &out, 0.5f * w + 10.f);
                                    polygons_append(bridged_perimeters, out);
                                }
                            }
                        }
                        bridged_perimeters = union_(bridged_perimeters);
                    }
                    
                    if (1) {
                        // remove the entire bridges and only support the unsupported edges
                        Polygons bridges;
                        for (Surfaces::const_iterator it = layerm.fill_surfaces.surfaces.begin(); it != layerm.fill_surfaces.surfaces.end(); ++ it)
                            if (it->surface_type == stBottomBridge && it->bridge_angle != -1)
                                polygons_append(bridges, it->expolygon);
                        polygons_append(bridged_perimeters, bridges);
                        diff_polygons = diff(diff_polygons, bridged_perimeters, true);

                        Polygons unsupported_bridge_polygons;                        
                        for (Polylines::const_iterator it = layerm.unsupported_bridge_edges.polylines.begin(); 
                            it != layerm.unsupported_bridge_edges.polylines.end(); ++ it) {
                            // Offset a polyline into a polygon.
                            Polylines tmp; tmp.push_back(*it);
                            Polygons out;
                            offset(tmp, &out, scale_(SUPPORT_MATERIAL_MARGIN));
                            polygons_append(unsupported_bridge_polygons, out);
                        }
                        polygons_append(diff_polygons, intersection(unsupported_bridge_polygons, bridges));
                    } else {
                        // just remove bridged areas
                        diff_polygons = diff(diff_polygons, layerm.bridged, true);
                    }
                } // if (m_objconfig->dont_support_bridges)

                if (buildplate_only) {
                    // Don't support overhangs above the top surfaces.
                    // This step is done before the contact surface is calculated by growing the overhang region.
                    diff_polygons = diff(diff_polygons, buildplate_only_top_surfaces);
                }

                if (diff_polygons.empty())
                    continue;

                #ifdef SLIC3R_DEBUG
                {
                    ::Slic3r::SVG svg(debug_out_path("support-top-contacts-filtered-run%d-layer%d-region%d.svg", iRun, layer_id, it_layerm - layer.regions.begin()), get_extents(diff_polygons));
                    Slic3r::ExPolygons expolys = union_ex(diff_polygons, false);
                    svg.draw(expolys);
                }
                #endif /* SLIC3R_DEBUG */

                polygons_append(overhang_polygons, diff_polygons);

                // Let's define the required contact area by using a max gap of half the upper 
                // extrusion width and extending the area according to the configured margin.
                // We increment the area in steps because we don't want our support to overflow
                // on the other side of the object (if it's very thin).
                {
                    //FIMXE 1) Make the offset configurable, 2) Make the Z span configurable.
                    Polygons slices_margin = offset((Polygons)lower_layer.slices, float(0.5*fw));
                    if (buildplate_only) {
                        // Trim the inflated contact surfaces by the top surfaces as well.
                        polygons_append(slices_margin, buildplate_only_top_surfaces);
                        slices_margin = union_(slices_margin);
                    }
                    // Offset the contact polygons outside.
                    for (size_t i = 0; i < NUM_MARGIN_STEPS; ++ i) {
                        diff_polygons = diff(
                            offset(
                                diff_polygons,
                                SUPPORT_MATERIAL_MARGIN / NUM_MARGIN_STEPS,
                                CLIPPER_OFFSET_SCALE,
                                ClipperLib::jtRound,
                                // round mitter limit
                                scale_(0.05) * CLIPPER_OFFSET_SCALE),
                            slices_margin);
                    }
                }
                polygons_append(contact_polygons, diff_polygons);
            } // for each layer.region
        } // end of Generate overhang/contact_polygons for non-raft layers.
        
        // now apply the contact areas to the layer were they need to be made
        if (! contact_polygons.empty()) {
            // get the average nozzle diameter used on this layer
            MyLayer &new_layer = layer_allocate(layer_storage, sltTopContact);
            new_layer.idx_object_layer_above = layer_id;
            if (m_soluble_interface) {
                // Align the contact surface height with a layer immediately below the supported layer.
                new_layer.height = (layer_id > 0) ? 
                    // Interface layer will be synchronized with the object.
                    object.get_layer(layer_id - 1)->height : 
                    // Don't know the thickness of the raft layer yet.
                    0.;
                new_layer.print_z = layer.print_z - layer.height;
                new_layer.bottom_z = new_layer.print_z - new_layer.height;
            } else {
                // Contact layer will be printed with a normal flow, but
                // it will support layers printed with a bridging flow.
                //FIXME Probably printing with the bridge flow? How about the unsupported perimeters? Are they printed with the bridging flow?
                // In the future we may switch to a normal extrusion flow for the supported bridges.
                // Get the average nozzle diameter used on this layer.
                coordf_t nozzle_dmr = 0.;
                size_t   n_nozzle_dmrs = 0;
                for (LayerRegionPtrs::const_iterator it_region_ptr = layer.regions.begin(); it_region_ptr != layer.regions.end(); ++ it_region_ptr) {
                    const PrintRegion &region = *(*it_region_ptr)->region();
                    nozzle_dmr += m_print_config->nozzle_diameter.get_at(region.config.perimeter_extruder.value - 1);
                    nozzle_dmr += m_print_config->nozzle_diameter.get_at(region.config.infill_extruder.value - 1);
                    nozzle_dmr += m_print_config->nozzle_diameter.get_at(region.config.solid_infill_extruder.value - 1);
                    n_nozzle_dmrs += 3;
                }
                nozzle_dmr /= coordf_t(n_nozzle_dmrs);
                new_layer.print_z = layer.print_z - nozzle_dmr - m_object_config->support_material_contact_distance;
                // Don't know the height of the top contact layer yet. The top contact layer is printed with a normal flow and 
                // its height will be set adaptively later on.
                new_layer.height = 0.;
                new_layer.bottom_z = new_layer.print_z;
            }

            // Ignore this contact area if it's too low.
            // Don't want to print a layer below the first layer height as it may not stick well.
            //FIXME there may be a need for a single layer support, then one may decide to print it either as a bottom contact or a top contact
            // and it may actually make sense to do it with a thinner layer than the first layer height.
            if (new_layer.print_z < this->first_layer_height() + m_support_layer_height_min)
                continue;

            new_layer.polygons.swap(contact_polygons);
            // Store the overhang polygons as the aux_polygons.
            // The overhang polygons are used in the path generator for planning of the contact circles.
            new_layer.aux_polygons = new Polygons();
            new_layer.aux_polygons->swap(overhang_polygons);
            contact_out.push_back(&new_layer);

            if (0) {
                // Slic3r::SVG::output("out\\contact_" . $contact_z . ".svg",
                //     green_expolygons => union_ex($buildplate_only_top_surfaces),
                //     blue_expolygons  => union_ex(\@contact),
                //     red_expolygons   => union_ex(\@overhang),
                // );
            }
        }
    }

    return contact_out;
}

PrintObjectSupportMaterial::MyLayersPtr PrintObjectSupportMaterial::bottom_contact_layers(
    const PrintObject &object, const MyLayersPtr &top_contacts, MyLayerStorage &layer_storage) const
{
    // find object top surfaces
    // we'll use them to clip our support and detect where does it stick
    MyLayersPtr bottom_contacts;
    if (! m_object_config->support_material_buildplate_only.value && ! top_contacts.empty())
    {
        // Sum of unsupported contact areas above the current layer.print_z.
        Polygons  projection;
        // Last top contact layer visited when collecting the projection of contact areas.
        int       contact_idx = int(top_contacts.size()) - 1;
        for (int layer_id = int(object.total_layer_count()) - 2; layer_id >= 0; -- layer_id) {
            const Layer &layer = *object.get_layer(layer_id);
            Polygons top = collect_region_slices_by_type(layer, stTop);
            if (top.empty())
                continue;
            // Collect projections of all contact areas above or at the same level as this top surface.
            for (; contact_idx >= 0 && top_contacts[contact_idx]->print_z >= layer.print_z; -- contact_idx)
                polygons_append(projection, top_contacts[contact_idx]->polygons);
            // Now find whether any projection of the contact surfaces above layer.print_z not yet supported by any 
            // top surfaces above layer.print_z falls onto this top surface. 
            // touching are the contact surfaces supported exclusively by this top surfaaces.
            Polygons touching = intersection(projection, top);
            if (touching.empty())
                continue;
            // Allocate a new bottom contact layer.
            MyLayer &layer_new = layer_allocate(layer_storage, sltBottomContact);
            bottom_contacts.push_back(&layer_new);
            // Grow top surfaces so that interface and support generation are generated
            // with some spacing from object - it looks we don't need the actual
            // top shapes so this can be done here
            layer_new.height  = m_soluble_interface ? 
                // Align the interface layer with the object's layer height.
                object.get_layer(layer_id + 1)->height :
                // Place a bridge flow interface layer over the top surface.
                m_support_material_interface_flow.nozzle_diameter;
            layer_new.print_z = layer.print_z + layer_new.height + 
                (m_soluble_interface ? 0. : m_object_config->support_material_contact_distance.value);
            layer_new.bottom_z = layer.print_z;
            layer_new.idx_object_layer_below = layer_id;
            layer_new.bridging = ! m_soluble_interface;
            //FIXME how much to inflate the top surface?
            Polygons poly_new = offset(touching, float(m_support_material_flow.scaled_width()));
            layer_new.polygons.swap(poly_new);
            // Remove the areas that touched from the projection that will continue on next, lower, top surfaces.
            projection = diff(projection, touching);
        }
    }
    return bottom_contacts;
}

// Trim the top_contacts layers with the bottom_contacts layers if they overlap, so there would not be enough vertical space for both of them.
void PrintObjectSupportMaterial::trim_top_contacts_by_bottom_contacts(
    const PrintObject &object, const MyLayersPtr &bottom_contacts, MyLayersPtr &top_contacts) const
{
    size_t idx_top_first = 0;
    // For all bottom contact layers:
    for (size_t idx_bottom = 0; idx_bottom < bottom_contacts.size() && idx_top_first < top_contacts.size(); ++ idx_bottom) {
        const MyLayer &layer_bottom = *bottom_contacts[idx_bottom];
        // Find the first top layer overlapping with layer_bottom.
        while (idx_top_first < top_contacts.size() && top_contacts[idx_top_first]->print_z <= layer_bottom.print_z - layer_bottom.height)
            ++ idx_top_first;
        // For all top contact layers overlapping with the thick bottom contact layer:
        for (size_t idx_top = idx_top_first; idx_top < top_contacts.size(); ++ idx_top) {
            MyLayer &layer_top = *top_contacts[idx_top];
            coordf_t interface_z = m_soluble_interface ? 
                (layer_top.bottom_z + EPSILON) :
                (layer_top.bottom_z - m_support_layer_height_min);
            if (interface_z < layer_bottom.print_z) {
                // Layers overlap. Trim layer_top with layer_bottom.
                layer_top.polygons = diff(layer_top.polygons, layer_bottom.polygons);
            } else
                break;
        }
    }
}

PrintObjectSupportMaterial::MyLayersPtr PrintObjectSupportMaterial::raft_and_intermediate_support_layers(
    const PrintObject   &object,
    const MyLayersPtr   &bottom_contacts,
    const MyLayersPtr   &top_contacts,
    MyLayerStorage      &layer_storage,
    const coordf_t       max_object_layer_height) const
{
    MyLayersPtr intermediate_layers;

    // Collect and sort the extremes (bottoms of the top contacts and tops of the bottom contacts).
    std::vector<LayerExtreme> extremes;
    extremes.reserve(top_contacts.size() + bottom_contacts.size());
    for (size_t i = 0; i < top_contacts.size(); ++ i)
        // Bottoms of the top contact layers. In case of non-soluble supports,
        // the top contact layer thickness is not known yet.
        extremes.push_back(LayerExtreme(top_contacts[i], false));
    for (size_t i = 0; i < bottom_contacts.size(); ++ i)
        // Tops of the bottom contact layers.
        extremes.push_back(LayerExtreme(bottom_contacts[i], true));
    if (extremes.empty())
        return intermediate_layers;
    std::sort(extremes.begin(), extremes.end());

    // Top of the 0th layer.
    coordf_t top_z_0th = this->raft_base_height() + this->raft_interface_height();
    assert(extremes.front().z() > top_z_0th && extremes.front().z() >= this->first_layer_height());

    // Generate intermediate layers.
    // The first intermediate layer is the same as the 1st layer if there is no raft,
    // or the bottom of the first intermediate layer is aligned with the bottom of the raft contact layer.
    // Intermediate layers are always printed with a normal etrusion flow (non-bridging).
    for (size_t idx_extreme = 0; idx_extreme < extremes.size(); ++ idx_extreme) {
        LayerExtreme *extr1  = (idx_extreme == 0) ? NULL : &extremes[idx_extreme-1];
        coordf_t      extr1z = (extr1 == NULL) ? top_z_0th : extr1->z();
        LayerExtreme &extr2  = extremes[idx_extreme];
        coordf_t      extr2z = extr2.z();
        coordf_t      dist   = extr2z - extr1z;
        assert(dist > 0.);
        // Insert intermediate layers.
        size_t        n_layers_extra = size_t(ceil(dist / m_support_layer_height_max));
        coordf_t      step   = dist / coordf_t(n_layers_extra);
        if (! m_soluble_interface && extr2.layer->layer_type == sltTopContact) {
            assert(extr2.layer->height == 0.);
            // This is a top interface layer, which does not have a height assigned yet. Do it now.
            if (m_synchronize_support_layers_with_object) {
                //FIXME
                // Find the 
            }
            extr2.layer->height = step;
            extr2.layer->bottom_z = extr2.layer->print_z - step;
            -- n_layers_extra;
            if (extr2.layer->bottom_z < this->first_layer_height()) {
                // Split the span into two layers: the top layer up to the first layer height, 
                // and the new intermediate layer below.
                // 1) Adjust the bottom of this top layer.
                assert(n_layers_extra == 0);
                extr2.layer->bottom_z = extr2z = this->first_layer_height();
                extr2.layer->height = extr2.layer->print_z - extr2.layer->bottom_z;
                // 2) Insert a new intermediate layer.
                MyLayer &layer_new = layer_allocate(layer_storage, stlIntermediate);
                layer_new.bottom_z   = extr1z;
                layer_new.print_z    = this->first_layer_height();
                layer_new.height     = layer_new.print_z - layer_new.bottom_z;
                intermediate_layers.push_back(&layer_new);
                continue;
            }
        }
        if (n_layers_extra > 0 && extr1z + step < this->first_layer_height()) {
            MyLayer &layer_new = layer_allocate(layer_storage, stlIntermediate);
            layer_new.bottom_z   = extr1z;
            layer_new.print_z    = extr1z = this->first_layer_height();
            layer_new.height     = layer_new.print_z - layer_new.bottom_z;
            intermediate_layers.push_back(&layer_new);
            dist = extr2z - extr1z;
            assert(dist >= 0.);
            n_layers_extra = size_t(ceil(dist / m_support_layer_height_max));
            coordf_t step = dist / coordf_t(n_layers_extra);
        }
        for (size_t i = 0; i < n_layers_extra; ++ i) {
            MyLayer &layer_new = layer_allocate(layer_storage, stlIntermediate);
            layer_new.height     = step;
            layer_new.bottom_z   = (i + 1 == n_layers_extra) ? extr2z : extr1z + i * step;
            layer_new.print_z    = layer_new.bottom_z + step;
            intermediate_layers.push_back(&layer_new);
        }
    }

    return intermediate_layers;
}

// At this stage there shall be intermediate_layers allocated between bottom_contacts and top_contacts, but they have no polygons assigned.
// Also the bottom/top_contacts shall have a thickness assigned already.
void PrintObjectSupportMaterial::generate_base_layers(
    const PrintObject   &object,
    const MyLayersPtr   &bottom_contacts,
    const MyLayersPtr   &top_contacts,
    MyLayersPtr         &intermediate_layers) const
{
    if (top_contacts.empty())
        // No top contacts -> no intermediate layers will be produced.
        return;

    // coordf_t fillet_radius_scaled = scale_(m_object_config->support_material_spacing);
    //FIXME make configurable:
    coordf_t overlap_extra_above = 0.01;
    coordf_t overlap_extra_below = 0.01;

    int idx_top_contact_above = int(top_contacts.size()) - 1;
    int idx_top_contact_overlapping = int(top_contacts.size()) - 1;
    int idx_bottom_contact_overlapping = int(bottom_contacts.size()) - 1;
    for (int idx_intermediate = int(intermediate_layers.size()) - 1; idx_intermediate >= 0; -- idx_intermediate)
    {
        MyLayer &layer_intermediate = *intermediate_layers[idx_intermediate];

        // New polygons for layer_intermediate.
        Polygons polygons_new;

        // Find a top_contact layer touching the layer_intermediate from above, if any, and collect its polygons into polygons_new.
        while (idx_top_contact_above >= 0 && top_contacts[idx_top_contact_above]->bottom_z > layer_intermediate.print_z + EPSILON)
            -- idx_top_contact_above;
        if (idx_top_contact_above >= 0 && top_contacts[idx_top_contact_above]->print_z > layer_intermediate.print_z)
            polygons_append(polygons_new, top_contacts[idx_top_contact_above]->polygons);

        // Add polygons from the intermediate layer above.
        if (idx_intermediate + 1 < int(intermediate_layers.size()))
            polygons_append(polygons_new, intermediate_layers[idx_intermediate+1]->polygons);

        // Polygons to trim polygons_new.
        Polygons polygons_trimming;

        // Find the first top_contact layer intersecting with this layer.
        while (idx_top_contact_overlapping >= 0 && 
               top_contacts[idx_top_contact_overlapping]->bottom_z > layer_intermediate.print_z + overlap_extra_above - EPSILON)
            -- idx_top_contact_overlapping;
        // Collect all the top_contact layer intersecting with this layer.
        for (int i = idx_top_contact_overlapping; i >= 0; -- i) {
			MyLayer &layer_top_overlapping = *top_contacts[i];
            if (layer_top_overlapping.print_z < layer_intermediate.bottom_z - overlap_extra_below)
                break;
            polygons_append(polygons_trimming, layer_top_overlapping.polygons);
        }

        // Find the first bottom_contact layer intersecting with this layer.
        while (idx_bottom_contact_overlapping >= 0 && 
            bottom_contacts[idx_bottom_contact_overlapping]->bottom_z > layer_intermediate.print_z + overlap_extra_above - EPSILON)
            -- idx_bottom_contact_overlapping;
        // Collect all the top_contact layer intersecting with this layer.
        for (int i = idx_bottom_contact_overlapping; i >= 0; -- i) {
            MyLayer &layer_bottom_overlapping = *bottom_contacts[idx_bottom_contact_overlapping];
            if (layer_bottom_overlapping.print_z < layer_intermediate.print_z - layer_intermediate.height - overlap_extra_below)
                break;
            polygons_append(polygons_trimming, layer_bottom_overlapping.polygons);
        }

        // Trim the polygons, store them.
        if (polygons_trimming.empty())
            layer_intermediate.polygons.swap(polygons_new);
        else
            layer_intermediate.polygons = diff(
                polygons_new,
                polygons_trimming,
                true); // safety offset to merge the touching source polygons

/*
        if (0) {
            // Fillet the base polygons and trim them again with the top, interface and contact layers.
            $base->{$i} = diff(
                offset2(
                    $base->{$i}, 
                    $fillet_radius_scaled, 
                    -$fillet_radius_scaled,
                    # Use a geometric offsetting for filleting.
                    CLIPPER_OFFSET_SCALE,
                    JT_ROUND,
                    0.2*$fillet_radius_scaled*CLIPPER_OFFSET_SCALE),
                $trim_polygons,
                false); // don't apply the safety offset.
        }
*/
    }

#ifdef SLIC3R_DEBUG
    static int iRun = 0;
    iRun ++;
    for (MyLayersPtr::const_iterator it = top_contacts.begin(); it != top_contacts.end(); ++ it) {
        const MyLayer &layer = *(*it);
        ::Slic3r::SVG svg(debug_out_path("support-intermediate-layers-untrimmed-%d-%lf.svg", iRun, layer.print_z), get_extents(layer.polygons));
        Slic3r::ExPolygons expolys = union_ex(layer.polygons, false);
        svg.draw(expolys);
    }
#endif /* SLIC3R_DEBUG */

    //FIXME This could be parallelized.
    const coordf_t gap_extra_above = 0.1f;
    const coordf_t gap_extra_below = 0.1f;
    const coord_t  gap_xy_scaled = m_support_material_flow.scaled_width();
    size_t idx_object_layer_overlapping = 0;
    // For all intermediate layers:
    for (MyLayersPtr::iterator it_layer = intermediate_layers.begin(); it_layer != intermediate_layers.end(); ++ it_layer) {
        MyLayer &layer_intermediate = *(*it_layer);
        if (layer_intermediate.polygons.empty())
            continue;
        // Find the overlapping object layers including the extra above / below gap.
        while (idx_object_layer_overlapping < object.layer_count() && 
            object.get_layer(idx_object_layer_overlapping)->print_z < layer_intermediate.print_z - layer_intermediate.height - gap_extra_below + EPSILON)
            ++ idx_object_layer_overlapping;
        // Collect all the object layers intersecting with this layer.
        Polygons polygons_trimming;
        for (int i = idx_object_layer_overlapping; i < object.layer_count(); ++ i) {
            const Layer &object_layer = *object.get_layer(i);
            if (object_layer.print_z > layer_intermediate.print_z + gap_extra_above - EPSILON)
                break;
            polygons_append(polygons_trimming, (Polygons)object_layer.slices);
        }

        // $layer->slices contains the full shape of layer, thus including
        // perimeter's width. $support contains the full shape of support
        // material, thus including the width of its foremost extrusion.
        // We leave a gap equal to a full extrusion width.
        layer_intermediate.polygons = diff(
            layer_intermediate.polygons,
            offset(polygons_trimming, gap_xy_scaled));
    }
}

Polygons PrintObjectSupportMaterial::generate_raft_base(
    const PrintObject   &object,
    const MyLayersPtr   &bottom_contacts,
    MyLayersPtr         &intermediate_layers) const
{
    assert(! bottom_contacts.empty());

    Polygons raft_polygons;
    #if 0
    const float inflate_factor = scale_(3.);
    if (this->has_raft()) {
        MyLayer &contacts      = *bottom_contacts.front();
        MyLayer &columns_base  = *intermediate_layers.front();
        if (m_num_base_raft_layers == 0 && m_num_interface_raft_layers == 0 && m_num_contact_raft_layers == 1) {
            // Having only the contact layer, which has the height of the 1st layer.
            // We are free to merge the contacts with the columns_base, they will be printed the same way.
            polygons_append(contacts.polygons, offset(columns_base.polygons, inflate_factor));
            contacts.polygons = union_(contacts.polygons);
        } else {
            // Having multiple raft layers.
            assert(m_num_interface_raft_layers > 0);
            // Extend the raft base by the bases of the support columns, add the raft contacts.
            raft_polygons = raft_interface_polygons;
            //FIXME make the offset configurable.
            polygons_append(raft_polygons, offset(columns_base.polygons, inflate_factor));
            raft_polygons = union_(raft_polygons);
        }
    } else {
        // No raft. The 1st intermediate layer contains the bases of the support columns.
        // Expand the polygons, but trim with the object.
        MyLayer &columns_base  = *intermediate_layers.front();
        columns_base.polygons = diff(
            offset(columns_base.polygons, inflate_factor),
            offset(m_object->get_layer(0), safety_factor);
    }
    #endif
    return raft_polygons;
}

// Convert some of the intermediate layers into top/bottom interface layers.
PrintObjectSupportMaterial::MyLayersPtr PrintObjectSupportMaterial::generate_interface_layers(
    const PrintObject   &object,
    const MyLayersPtr   &bottom_contacts,
    const MyLayersPtr   &top_contacts,
    MyLayersPtr         &intermediate_layers,
    MyLayerStorage      &layer_storage) const
{
    // Old comment:
    // Compute interface area on this layer as diff of upper contact area
    // (or upper interface area) and layer slices.
    // This diff is responsible of the contact between support material and
    // the top surfaces of the object. We should probably offset the top 
    // surfaces vertically before performing the diff, but this needs 
    // investigation.

//    my $area_threshold = $self->interface_flow->scaled_spacing ** 2;

    MyLayersPtr interface_layers;
    // Contact layer is considered an interface layer, therefore run the following block only if support_material_interface_layers > 1.
    if (! intermediate_layers.empty() && m_object_config->support_material_interface_layers > 1) {
        // Index of the first top contact layer intersecting the current intermediate layer.
        size_t idx_top_contact_first = 0;
        // Index of the first bottom contact layer intersecting the current intermediate layer.
        size_t idx_bottom_contact_first = 0;
        // For all intermediate layers, collect top contact surfaces, which are not further than support_material_interface_layers.
        //FIXME this could be parallelized.
        for (size_t idx_intermediate_layer = 0; idx_intermediate_layer < intermediate_layers.size(); ++ idx_intermediate_layer) {
            MyLayer &intermediate_layer = *intermediate_layers[idx_intermediate_layer];
            // Top / bottom Z coordinate of a slab, over which we are collecting the top / bottom contact surfaces.
            coordf_t top_z    = intermediate_layers[std::min<int>(intermediate_layers.size()-1, idx_intermediate_layer + m_object_config->support_material_interface_layers - 1)]->print_z;
            coordf_t bottom_z = intermediate_layers[std::max<int>(0, int(idx_intermediate_layer) - int(m_object_config->support_material_interface_layers) + 1)]->bottom_z;
            // Move idx_top_contact_first up until above the current print_z.
            while (idx_top_contact_first < top_contacts.size() && top_contacts[idx_top_contact_first]->print_z < intermediate_layer.print_z)
                ++ idx_top_contact_first;
            // Collect the top contact areas above this intermediate layer, below top_z.
            Polygons polygons_top_contact_projected;
            for (size_t idx_top_contact = idx_top_contact_first; idx_top_contact < top_contacts.size(); ++ idx_top_contact) {
                const MyLayer &top_contact_layer = *top_contacts[idx_top_contact];
                if (top_contact_layer.bottom_z - EPSILON > top_z)
                    break;
                polygons_append(polygons_top_contact_projected, top_contact_layer.polygons);
            }
            // Move idx_bottom_contact_first up until touching bottom_z.
            while (idx_bottom_contact_first < bottom_contacts.size() && bottom_contacts[idx_bottom_contact_first]->print_z + EPSILON < bottom_z)
                ++ idx_bottom_contact_first;
            // Collect the top contact areas above this intermediate layer, below top_z.
            Polygons polygons_bottom_contact_projected;
            for (size_t idx_bottom_contact = idx_bottom_contact_first; idx_bottom_contact < bottom_contacts.size(); ++ idx_bottom_contact) {
                const MyLayer &bottom_contact_layer = *bottom_contacts[idx_bottom_contact];
                if (bottom_contact_layer.print_z - EPSILON > intermediate_layer.bottom_z)
                    break;
                polygons_append(polygons_bottom_contact_projected, bottom_contact_layer.polygons);
            }

            if (polygons_top_contact_projected.empty() && polygons_bottom_contact_projected.empty())
                continue;

            // Insert a new layer into top_interface_layers.
            MyLayer &layer_new = layer_allocate(layer_storage, 
                polygons_top_contact_projected.empty() ? sltBottomInterface : sltTopInterface);
            layer_new.print_z    = intermediate_layer.print_z;
            layer_new.bottom_z   = intermediate_layer.bottom_z;
            layer_new.height     = intermediate_layer.height;
            layer_new.bridging   = intermediate_layer.bridging;
            interface_layers.push_back(&layer_new);

            polygons_append(polygons_top_contact_projected, polygons_bottom_contact_projected);
            polygons_top_contact_projected = union_(polygons_top_contact_projected, true);
            layer_new.polygons = intersection(intermediate_layer.polygons, polygons_top_contact_projected);
            //FIXME filter layer_new.polygons islands by a minimum area?
//                $interface_area = [ grep abs($_->area) >= $area_threshold, @$interface_area ];
            intermediate_layer.polygons = diff(intermediate_layer.polygons, polygons_top_contact_projected, false);
        }
    }
    
    return interface_layers;
}

void PrintObjectSupportMaterial::generate_toolpaths(
    const PrintObject   &object,
    const Polygons      &raft,
    const MyLayersPtr   &bottom_contacts,
    const MyLayersPtr   &top_contacts,
    const MyLayersPtr   &intermediate_layers,
    const MyLayersPtr   &interface_layers) const
{
    // Shape of the top contact area.
    int         n_contact_loops = 1;
    coordf_t    circle_radius   = 1.5 * m_support_material_interface_flow.scaled_width();
    coordf_t    circle_distance = 3. * circle_radius;
    Polygon     circle;
    circle.points.reserve(6);
    for (size_t i = 0; i < 6; ++ i) {
        double angle = double(i) * M_PI / 3.;
        circle.points.push_back(Point(circle_radius * cos(angle), circle_radius * sin(angle)));
    }
    
//    Slic3r::debugf "Generating patterns\n";

    // Prepare fillers.
    SupportMaterialPattern  support_pattern = m_object_config->support_material_pattern;
    bool                    with_sheath     = m_object_config->support_material_with_sheath;
    InfillPattern           infill_pattern;
    std::vector<double>     angles;
    angles.push_back(m_object_config->support_material_angle);
    switch (support_pattern) {
    case smpRectilinearGrid:
        angles.push_back(angles[0] + 90.);
        // fall through
    case smpRectilinear:
        infill_pattern = ipRectilinear;
        break;
    case smpHoneycomb:
    case smpPillars:
        infill_pattern = ipHoneycomb;
        break;
    }
#if SLIC3R_CPPVER >= 11
    std::unique_ptr<Fill> filler_interface = std::unique_ptr<Fill>(Fill::new_from_type(ipRectilinear));
    std::unique_ptr<Fill> filler_support   = std::unique_ptr<Fill>(Fill::new_from_type(infill_pattern));
#else
    std::auto_ptr<Fill> filler_interface = std::auto_ptr<Fill>(Fill::new_from_type(ipRectilinear));
    std::auto_ptr<Fill> filler_support   = std::auto_ptr<Fill>(Fill::new_from_type(infill_pattern));
#endif
    {
        BoundingBox bbox_object = object.bounding_box();
        filler_interface->set_bounding_box(bbox_object);
        filler_support->set_bounding_box(bbox_object);
    }

    coordf_t interface_angle    = m_object_config->support_material_angle + 90.;
    coordf_t interface_spacing  = m_object_config->support_material_interface_spacing.value + m_support_material_interface_flow.spacing();
    coordf_t interface_density  = (interface_spacing == 0.) ? 1. : (m_support_material_interface_flow.spacing() / interface_spacing);
    coordf_t support_spacing    = m_object_config->support_material_spacing.value + m_support_material_flow.spacing();
    coordf_t support_density    = (support_spacing == 0.) ? 1. : (m_support_material_flow.spacing() / support_spacing);
    
    //FIXME Parallelize the support generator:
    /*
    Slic3r::parallelize(
        threads => $self->print_config->threads,
        items => [ 0 .. n_$object.support_layers} ],
        thread_cb => sub {
            my $q = shift;
            while (defined (my $layer_id = $q->dequeue)) {
                $process_layer->($layer_id);
            }
        },
        no_threads_cb => sub {
            $process_layer->($_) for 0 .. n_{$object.support_layers};
        },
    );
    */
    // Indices of the 1st layer in their respective container at the support layer height.
    size_t idx_layer_bottom_contact   = 0;
    size_t idx_layer_top_contact      = 0;
    size_t idx_layer_intermediate     = 0;
    size_t idx_layer_inteface         = 0;
    for (size_t support_layer_id = 0; support_layer_id < object.support_layers.size(); ++ support_layer_id) 
    {
        SupportLayer &support_layer = *object.support_layers[support_layer_id];

        // Find polygons with the same print_z.
        Polygons bottom_contact_polygons;
        Polygons top_contact_polygons;
        Polygons base_polygons;
        Polygons interface_polygons;

        // Increment the layer indices to find a layer at support_layer.print_z.
        for (; idx_layer_bottom_contact < bottom_contacts    .size() && bottom_contacts    [idx_layer_bottom_contact]->print_z < support_layer.print_z - EPSILON; ++ idx_layer_bottom_contact) ;
        for (; idx_layer_top_contact    < top_contacts       .size() && top_contacts       [idx_layer_top_contact   ]->print_z < support_layer.print_z - EPSILON; ++ idx_layer_top_contact   ) ;
        for (; idx_layer_intermediate   < intermediate_layers.size() && intermediate_layers[idx_layer_intermediate  ]->print_z < support_layer.print_z - EPSILON; ++ idx_layer_intermediate  ) ;
        for (; idx_layer_inteface       < interface_layers   .size() && interface_layers   [idx_layer_inteface      ]->print_z < support_layer.print_z - EPSILON; ++ idx_layer_inteface      ) ;
        // Copy polygons from the layers.
        if (idx_layer_bottom_contact < bottom_contacts.size() && bottom_contacts[idx_layer_bottom_contact]->print_z < support_layer.print_z + EPSILON)
            bottom_contact_polygons = bottom_contacts[idx_layer_bottom_contact]->polygons;
        if (idx_layer_top_contact < top_contacts.size() && top_contacts[idx_layer_top_contact]->print_z < support_layer.print_z + EPSILON)
            top_contact_polygons = top_contacts[idx_layer_top_contact]->polygons;
        if (idx_layer_inteface < interface_layers.size() && interface_layers[idx_layer_inteface]->print_z < support_layer.print_z + EPSILON)
            interface_polygons = interface_layers[idx_layer_inteface]->polygons;
        if (idx_layer_intermediate < intermediate_layers.size() && intermediate_layers[idx_layer_intermediate]->print_z < support_layer.print_z + EPSILON)
            base_polygons = intermediate_layers[idx_layer_intermediate]->polygons;
        
        // We redefine flows locally by applying this layer's height.
        Flow flow           = m_support_material_flow;
        Flow interface_flow = m_support_material_interface_flow;
        flow.height         = support_layer.height;
        interface_flow.height = support_layer.height;
        
        /*
        if (1) {
            require "Slic3r/SVG.pm";
            Slic3r::SVG::output("out\\layer_" . $z . ".svg",
                blue_expolygons     => union_ex($base),
                red_expolygons      => union_ex($contact),
                green_expolygons    => union_ex($interface),
            );
        }
        */
        
        // Store inslands, over which the retract will be disabled.
        {
            Polygons polys(bottom_contact_polygons);
            polygons_append(polys, interface_polygons);
            polygons_append(polys, base_polygons);
            polygons_append(polys, top_contact_polygons);
            ExPolygons islands = union_ex(polys);
            support_layer.support_islands.expolygons.insert(support_layer.support_islands.expolygons.end(), islands.begin(), islands.end());
        }
        
        Polygons contact_infill_polygons;
        if (! top_contact_polygons.empty())
        {
            // Having a top interface layer.
            if (m_object_config->support_material_interface_layers == 0)
                // If no interface layers were requested, we treat the contact layer exactly as a generic base layer.
                polygons_append(base_polygons, top_contact_polygons);
            else if (n_contact_loops == 0)
                // If no loops are allowed, we treat the contact layer exactly as a generic interface layer.
                polygons_append(interface_polygons, top_contact_polygons);
            else if (! top_contact_polygons.empty())
            {
                // Create loop paths and 
                Polygons overhang_polygons = (top_contacts[idx_layer_top_contact]->aux_polygons == NULL) ? 
                    Polygons() :
                    *top_contacts[idx_layer_top_contact]->aux_polygons;

                // Generate the outermost loop.
                // Find centerline of the external loop (or any other kind of extrusions should the loop be skipped)
                top_contact_polygons = offset(top_contact_polygons, - 0.5 * interface_flow.scaled_width());
                
                Polygons loops0;
                {
                    // find centerline of the external loop of the contours
                    // only consider the loops facing the overhang
                    Polygons external_loops;
                    // Positions of the loop centers.
                    Polygons circles;
                    {
                        Polygons overhang_with_margin = offset(overhang_polygons, 0.5 * interface_flow.scaled_width());
                        for (Polygons::const_iterator it_contact = top_contact_polygons.begin(); it_contact != top_contact_polygons.end(); ++ it_contact) {
                            Polylines tmp;
                            tmp.push_back(it_contact->split_at_first_point());
                            if (! intersection(tmp, overhang_with_margin).empty()) {
                                external_loops.push_back(*it_contact);
                                Points positions_new = it_contact->equally_spaced_points(circle_distance);
                                for (Points::const_iterator it_center = positions_new.begin(); it_center != positions_new.end(); ++ it_center) {
                                    circles.push_back(circle);
                                    Polygon &circle_new = circles.back();
                                    for (size_t i = 0; i < circle_new.points.size(); ++ i)
                                        circle_new.points[i].translate(*it_center);
                                }
                            }
                        }
                    }
                    // Apply a pattern to the loop.
                    loops0 = diff(external_loops, circles);
                }

                Polylines loop_lines;
                {
                    // make more loops
                    Polygons loop_polygons = loops0;
                    for (size_t i = 1; i < n_contact_loops; ++ i)
                        polygons_append(loop_polygons, 
                            offset2(
                                loops0, 
                                - int(i) * interface_flow.scaled_spacing() - 0.5 * interface_flow.scaled_spacing(), 
                                0.5 * interface_flow.scaled_spacing()));
                    // clip such loops to the side oriented towards the object
                    loop_lines.reserve(loop_polygons.size());
                    for (Polygons::const_iterator it = loop_polygons.begin(); it != loop_polygons.end(); ++ it)
                        loop_lines.push_back(it->split_at_first_point());
                    loop_lines = intersection(loop_lines, offset(overhang_polygons, scale_(SUPPORT_MATERIAL_MARGIN)));
                }
                
                // add the contact infill area to the interface area
                // note that growing loops by $circle_radius ensures no tiny
                // extrusions are left inside the circles; however it creates
                // a very large gap between loops and contact_infill_polygons, so maybe another
                // solution should be found to achieve both goals
                {
                    Polygons loop_polygons;
                    offset(loop_lines, &loop_polygons, circle_radius * 1.1);
                    contact_infill_polygons = diff(top_contact_polygons, loop_polygons);
                }

                // Transform loops into ExtrusionPath objects.
                for (Polylines::const_iterator it_polyline = loop_lines.begin(); it_polyline != loop_lines.end(); ++ it_polyline) {
                    ExtrusionPath *extrusion_path = new ExtrusionPath(erSupportMaterialInterface);
                    support_layer.support_interface_fills.entities.push_back(extrusion_path);
                    extrusion_path->polyline    = *it_polyline;
                    extrusion_path->mm3_per_mm  = interface_flow.mm3_per_mm();
                    extrusion_path->width       = interface_flow.width;
                    extrusion_path->height      = support_layer.height;
                }
            }
        }

        // interface and contact infill
        if (! interface_polygons.empty() || ! contact_infill_polygons.empty()) {
            //FIXME When paralellizing, each thread shall have its own copy of the fillers.
            filler_interface->angle   = interface_angle;
            filler_interface->spacing = interface_flow.spacing();
            
            // find centerline of the external loop
            interface_polygons = offset2(interface_polygons, SCALED_EPSILON, - SCALED_EPSILON - 0.5 * interface_flow.scaled_width());
            // join regions by offsetting them to ensure they're merged
            polygons_append(interface_polygons, contact_infill_polygons);
            interface_polygons = offset(interface_polygons, SCALED_EPSILON);
            
            // turn base support into interface when it's contained in our holes
            // (this way we get wider interface anchoring)
            {
                Polygons interface_polygons_new;
                interface_polygons_new.reserve(interface_polygons.size());
                for (Polygons::iterator it_polygon = interface_polygons.begin(); it_polygon != interface_polygons.end(); ++ it_polygon) {
                    if (it_polygon->is_clockwise()) {
                        Polygons hole;
                        hole.push_back(*it_polygon);
                        hole.back().make_counter_clockwise();
                        if (diff(hole, base_polygons, true).empty())
                            continue;
                    }
                    interface_polygons_new.push_back(Polygon());
                    interface_polygons_new.back().points.swap(it_polygon->points);
                }
                interface_polygons.swap(interface_polygons_new);
            }
            base_polygons = diff(base_polygons, interface_polygons);

            ExPolygons to_fill = union_ex(interface_polygons);
            for (ExPolygons::const_iterator it_expolygon = to_fill.begin(); it_expolygon != to_fill.end(); ++ it_expolygon) {
                Surface surface(stInternal, *it_expolygon);
                FillParams fill_params;
                fill_params.density = interface_density;
                fill_params.complete = true;
                Polylines polylines = filler_interface->fill_surface(&surface, fill_params);
                for (Polylines::const_iterator it_polyline = polylines.begin(); it_polyline != polylines.end(); ++ it_polyline) {
                    ExtrusionPath *extrusion_path = new ExtrusionPath(erSupportMaterialInterface);
                    support_layer.support_interface_fills.entities.push_back(extrusion_path);
                    extrusion_path->polyline    = *it_polyline;
                    extrusion_path->mm3_per_mm  = interface_flow.mm3_per_mm();
                    extrusion_path->width       = interface_flow.width;
                    extrusion_path->height      = support_layer.height;
                }
            }
        }
        
        // support or flange
        if (! base_polygons.empty()) {
            //FIXME When paralellizing, each thread shall have its own copy of the fillers.
            Fill *filler = filler_support.get();
            filler->angle = angles[support_layer_id % angles.size()];
            // We don't use $base_flow->spacing because we need a constant spacing
            // value that guarantees that all layers are correctly aligned.
            filler->spacing = flow.spacing();
            
            coordf_t density     = support_density;
            Flow     base_flow   = flow;
            
            // find centerline of the external loop/extrusions
            ExPolygons to_infill = offset2_ex(base_polygons, SCALED_EPSILON, - SCALED_EPSILON - 0.5*flow.scaled_width());

            /*
            if (1) {
                require "Slic3r/SVG.pm";
                Slic3r::SVG::output("out\\to_infill_base" . $z . ".svg",
                    red_expolygons      => union_ex($contact),
                    green_expolygons    => union_ex($interface),
                    blue_expolygons     => $to_infill,
                );
            }
            */
            
            if (support_layer_id == 0) {
                // Base flange.
                filler = filler_interface.get();
                filler->angle = m_object_config->support_material_angle + 90.;
                density   = 0.5;
                base_flow = m_first_layer_flow;
                // use the proper spacing for first layer as we don't need to align
                // its pattern to the other layers
                //FIXME When paralellizing, each thread shall have its own copy of the fillers.
                filler->spacing = base_flow.spacing();
            } else if (with_sheath) {
                // Draw a perimeter all around the support infill. This makes the support stable, but difficult to remove.
                // TODO: use brim ordering algorithm
                Polygons to_infill_polygons = to_polygons(to_infill);
                for (Polygons::const_iterator it_polyline = to_infill_polygons.begin(); it_polyline != to_infill_polygons.end(); ++ it_polyline) {
                    ExtrusionPath *extrusion_path = new ExtrusionPath(erSupportMaterial);
                    support_layer.support_fills.entities.push_back(extrusion_path);
                    extrusion_path->polyline    = *it_polyline;
                    extrusion_path->mm3_per_mm  = flow.mm3_per_mm();
                    extrusion_path->width       = flow.width;
                    extrusion_path->height      = support_layer.height;
                }
                // TODO: use offset2_ex()
                to_infill = offset_ex(to_infill_polygons, - flow.scaled_spacing());
            }
            
            for (ExPolygons::const_iterator it_expolygon = to_infill.begin(); it_expolygon != to_infill.end(); ++ it_expolygon) {
                Surface surface(stInternal, *it_expolygon);
                FillParams fill_params;
                fill_params.density = density;
                fill_params.complete = true;
                Polylines polylines = filler->fill_surface(&surface, fill_params);
                for (Polylines::const_iterator it_polyline = polylines.begin(); it_polyline != polylines.end(); ++ it_polyline) {
                    ExtrusionPath *extrusion_path = new ExtrusionPath(erSupportMaterial);
                    support_layer.support_fills.entities.push_back(extrusion_path);
                    extrusion_path->polyline    = *it_polyline;
                    extrusion_path->mm3_per_mm  = base_flow.mm3_per_mm();
                    extrusion_path->width       = base_flow.width;
                    extrusion_path->height      = support_layer.height;
                }
            }
        }

        // support or flange
        if (! bottom_contact_polygons.empty()) {
            //FIXME When paralellizing, each thread shall have its own copy of the fillers.
            Fill *filler = filler_support.get();
            filler->angle = angles[support_layer_id % angles.size()];
            // We don't use $base_flow->spacing because we need a constant spacing
            // value that guarantees that all layers are correctly aligned.
            filler->spacing = flow.spacing();
            
            coordf_t density     = support_density;
            Flow     base_flow   = flow;
            
            // find centerline of the external loop/extrusions
            ExPolygons to_infill = offset2_ex(base_polygons, SCALED_EPSILON, - SCALED_EPSILON - 0.5*flow.scaled_width());

            /*
            if (1) {
                require "Slic3r/SVG.pm";
                Slic3r::SVG::output("out\\to_infill_base" . $z . ".svg",
                    red_expolygons      => union_ex($contact),
                    green_expolygons    => union_ex($interface),
                    blue_expolygons     => $to_infill,
                );
            }
            */
            
            if (support_layer_id == 0) {
                // Base flange.
                filler = filler_interface.get();
                filler->angle = m_object_config->support_material_angle + 90.;
                density   = 0.5;
                base_flow = m_first_layer_flow;
                // use the proper spacing for first layer as we don't need to align
                // its pattern to the other layers
                //FIXME When paralellizing, each thread shall have its own copy of the fillers.
                filler->spacing = base_flow.spacing();
            } else if (with_sheath) {
                // Draw a perimeter all around the support infill. This makes the support stable, but difficult to remove.
                // TODO: use brim ordering algorithm
                Polygons to_infill_polygons = to_polygons(to_infill);
                for (Polygons::const_iterator it_polyline = to_infill_polygons.begin(); it_polyline != to_infill_polygons.end(); ++ it_polyline) {
                    ExtrusionPath *extrusion_path = new ExtrusionPath(erSupportMaterial);
                    support_layer.support_fills.entities.push_back(extrusion_path);
                    extrusion_path->polyline    = *it_polyline;
                    extrusion_path->mm3_per_mm  = flow.mm3_per_mm();
                    extrusion_path->width       = flow.width;
                    extrusion_path->height      = support_layer.height;
                }
                // TODO: use offset2_ex()
                to_infill = offset_ex(to_infill_polygons, - flow.scaled_spacing());
            }
            
            for (ExPolygons::const_iterator it_expolygon = to_infill.begin(); it_expolygon != to_infill.end(); ++ it_expolygon) {
                Surface surface(stInternal, *it_expolygon);
                FillParams fill_params;
                fill_params.density = density;
                fill_params.complete = true;
                Polylines polylines = filler->fill_surface(&surface, fill_params);
                for (Polylines::const_iterator it_polyline = polylines.begin(); it_polyline != polylines.end(); ++ it_polyline) {
                    ExtrusionPath *extrusion_path = new ExtrusionPath(erSupportMaterial);
                    support_layer.support_fills.entities.push_back(extrusion_path);
                    extrusion_path->polyline    = *it_polyline;
                    extrusion_path->mm3_per_mm  = base_flow.mm3_per_mm();
                    extrusion_path->width       = base_flow.width;
                    extrusion_path->height      = support_layer.height;
                }
            }
        }

        /*
        if (0) {
            require "Slic3r/SVG.pm";
            Slic3r::SVG::output("islands_" . $z . ".svg",
                red_expolygons      => union_ex($contact),
                green_expolygons    => union_ex($interface),
                green_polylines     => [ map $_->unpack->polyline, @{$layer->support_contact_fills} ],
                polylines           => [ map $_->unpack->polyline, @{$layer->support_fills} ],
            );
        }
        */
    } // for each support_layer_id
}

/*
void PrintObjectSupportMaterial::clip_by_pillars(
    const PrintObject   &object,
    LayersPtr           &bottom_contacts,
    LayersPtr           &top_contacts,
    LayersPtr           &intermediate_contacts);

{
    // this prevents supplying an empty point set to BoundingBox constructor
    if (top_contacts.empty())
        return;

    coord_t pillar_size    = scale_(PILLAR_SIZE);
    coord_t pillar_spacing = scale_(PILLAR_SPACING);
    
    // A regular grid of pillars, filling the 2D bounding box.
    Polygons grid;
    {
        // Rectangle with a side of 2.5x2.5mm.
        Polygon pillar;
        pillar.points.push_back(Point(0, 0));
        pillar.points.push_back(Point(pillar_size, 0));
        pillar.points.push_back(Point(pillar_size, pillar_size));
        pillar.points.push_back(Point(0, pillar_size));
        
        // 2D bounding box of the projection of all contact polygons.
        BoundingBox bbox;
        for (LayersPtr::const_iterator it = top_contacts.begin(); it != top_contacts.end(); ++ it)
            bbox.merge(get_extents((*it)->polygons));
        grid.reserve(size_t(ceil(bb.size().x / pillar_spacing)) * size_t(ceil(bb.size().y / pillar_spacing)));
        for (coord_t x = bb.min.x; x <= bb.max.x - pillar_size; x += pillar_spacing) {
            for (coord_t y = bb.min.y; y <= bb.max.y - pillar_size; y += pillar_spacing) {
                grid.push_back(pillar);
                for (size_t i = 0; i < pillar.points.size(); ++ i)
                    grid.back().points[i].translate(Point(x, y));
            }
        }
    }
    
    // add pillars to every layer
    for my $i (0..n_support_z) {
        $shape->[$i] = [ @$grid ];
    }
    
    // build capitals
    for my $i (0..n_support_z) {
        my $z = $support_z->[$i];
        
        my $capitals = intersection(
            $grid,
            $contact->{$z} // [],
        );
        
        // work on one pillar at time (if any) to prevent the capitals from being merged
        // but store the contact area supported by the capital because we need to make 
        // sure nothing is left
        my $contact_supported_by_capitals = [];
        foreach my $capital (@$capitals) {
            // enlarge capital tops
            $capital = offset([$capital], +($pillar_spacing - $pillar_size)/2);
            push @$contact_supported_by_capitals, @$capital;
            
            for (my $j = $i-1; $j >= 0; $j--) {
                my $jz = $support_z->[$j];
                $capital = offset($capital, -$self->interface_flow->scaled_width/2);
                last if !@$capitals;
                push @{ $shape->[$j] }, @$capital;
            }
        }
        
        // Capitals will not generally cover the whole contact area because there will be
        // remainders. For now we handle this situation by projecting such unsupported
        // areas to the ground, just like we would do with a normal support.
        my $contact_not_supported_by_capitals = diff(
            $contact->{$z} // [],
            $contact_supported_by_capitals,
        );
        if (@$contact_not_supported_by_capitals) {
            for (my $j = $i-1; $j >= 0; $j--) {
                push @{ $shape->[$j] }, @$contact_not_supported_by_capitals;
            }
        }
    }
}

sub clip_with_shape {
    my ($self, $support, $shape) = @_;
    
    foreach my $i (keys %$support) {
        // don't clip bottom layer with shape so that we 
        // can generate a continuous base flange
        // also don't clip raft layers
        next if $i == 0;
        next if $i < $self->object_config->raft_layers;
        $support->{$i} = intersection(
            $support->{$i},
            $shape->[$i],
        );
    }
}
*/

} // namespace Slic3r

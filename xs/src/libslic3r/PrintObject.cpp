#include "Print.hpp"
#include "BoundingBox.hpp"
#include "ClipperUtils.hpp"
#include "Geometry.hpp"
#include "SupportMaterial.hpp"

#include <utility>
#include <boost/log/trivial.hpp>

#include <tbb/task_scheduler_init.h>
#include <tbb/parallel_for.h>
#include <tbb/atomic.h>

#include <Shiny/Shiny.h>

#ifdef SLIC3R_DEBUG_SLICE_PROCESSING
#define SLIC3R_DEBUG
#endif

// #define SLIC3R_DEBUG

// Make assert active if SLIC3R_DEBUG
#ifdef SLIC3R_DEBUG
    #undef NDEBUG
    #define DEBUG
    #define _DEBUG
    #include "SVG.hpp"
    #undef assert 
    #include <cassert>
#endif

namespace Slic3r {

PrintObject::PrintObject(Print* print, ModelObject* model_object, const BoundingBoxf3 &modobj_bbox)
:   typed_slices(false),
    _print(print),
    _model_object(model_object),
    layer_height_profile_valid(false)
{
    // Compute the translation to be applied to our meshes so that we work with smaller coordinates
    {
        // Translate meshes so that our toolpath generation algorithms work with smaller
        // XY coordinates; this translation is an optimization and not strictly required.
        // A cloned mesh will be aligned to 0 before slicing in _slice_region() since we
        // don't assume it's already aligned and we don't alter the original position in model.
        // We store the XY translation so that we can place copies correctly in the output G-code
        // (copies are expressed in G-code coordinates and this translation is not publicly exposed).
        this->_copies_shift = Point(
            scale_(modobj_bbox.min.x), scale_(modobj_bbox.min.y));

        // Scale the object size and store it
        Pointf3 size = modobj_bbox.size();
        this->size = Point3(scale_(size.x), scale_(size.y), scale_(size.z));
    }
    
    this->reload_model_instances();
    this->layer_height_ranges = model_object->layer_height_ranges;
    this->layer_height_profile = model_object->layer_height_profile;
}

bool
PrintObject::add_copy(const Pointf &point)
{
    Points points = this->_copies;
    points.push_back(Point::new_scale(point.x, point.y));
    return this->set_copies(points);
}

bool
PrintObject::delete_last_copy()
{
    Points points = this->_copies;
    points.pop_back();
    return this->set_copies(points);
}

bool
PrintObject::delete_all_copies()
{
    Points points;
    return this->set_copies(points);
}

bool
PrintObject::set_copies(const Points &points)
{
    this->_copies = points;
    
    // order copies with a nearest neighbor search and translate them by _copies_shift
    this->_shifted_copies.clear();
    this->_shifted_copies.reserve(points.size());
    
    // order copies with a nearest-neighbor search
    std::vector<Points::size_type> ordered_copies;
    Slic3r::Geometry::chained_path(points, ordered_copies);
    
    for (std::vector<Points::size_type>::const_iterator it = ordered_copies.begin(); it != ordered_copies.end(); ++it) {
        Point copy = points[*it];
        copy.translate(this->_copies_shift);
        this->_shifted_copies.push_back(copy);
    }
    
    bool invalidated = false;
    if (this->_print->invalidate_step(psSkirt)) invalidated = true;
    if (this->_print->invalidate_step(psBrim)) invalidated = true;
    return invalidated;
}

bool
PrintObject::reload_model_instances()
{
    Points copies;
    for (ModelInstancePtrs::const_iterator i = this->_model_object->instances.begin(); i != this->_model_object->instances.end(); ++i) {
        copies.push_back(Point::new_scale((*i)->offset.x, (*i)->offset.y));
    }
    return this->set_copies(copies);
}

void
PrintObject::add_region_volume(int region_id, int volume_id)
{
    region_volumes[region_id].push_back(volume_id);
}

/*  This is the *total* layer count (including support layers)
    this value is not supposed to be compared with Layer::id
    since they have different semantics */
size_t
PrintObject::total_layer_count() const
{
    return this->layer_count() + this->support_layer_count();
}

size_t
PrintObject::layer_count() const
{
    return this->layers.size();
}

void
PrintObject::clear_layers()
{
    for (size_t i = 0; i < this->layers.size(); ++ i) {
        Layer *layer = this->layers[i];
        layer->upper_layer = layer->lower_layer = nullptr;
        delete layer;
    }
    this->layers.clear();
}

Layer*
PrintObject::add_layer(int id, coordf_t height, coordf_t print_z, coordf_t slice_z)
{
    Layer* layer = new Layer(id, this, height, print_z, slice_z);
    layers.push_back(layer);
    return layer;
}

void
PrintObject::delete_layer(int idx)
{
    LayerPtrs::iterator i = this->layers.begin() + idx;
    delete *i;
    this->layers.erase(i);
}

size_t
PrintObject::support_layer_count() const
{
    return this->support_layers.size();
}

void
PrintObject::clear_support_layers()
{
    for (size_t i = 0; i < this->support_layers.size(); ++ i) {
        Layer *layer = this->support_layers[i];
        layer->upper_layer = layer->lower_layer = nullptr;
        delete layer;
    }
    this->support_layers.clear();
}

SupportLayer*
PrintObject::get_support_layer(int idx)
{
    return this->support_layers.at(idx);
}

SupportLayer*
PrintObject::add_support_layer(int id, coordf_t height, coordf_t print_z)
{
    SupportLayer* layer = new SupportLayer(id, this, height, print_z, -1);
    support_layers.push_back(layer);
    return layer;
}

void
PrintObject::delete_support_layer(int idx)
{
    SupportLayerPtrs::iterator i = this->support_layers.begin() + idx;
    delete *i;
    this->support_layers.erase(i);
}

bool
PrintObject::invalidate_state_by_config_options(const std::vector<t_config_option_key> &opt_keys)
{
    std::set<PrintObjectStep> steps;
    
    // this method only accepts PrintObjectConfig and PrintRegionConfig option keys
    for (std::vector<t_config_option_key>::const_iterator opt_key = opt_keys.begin(); opt_key != opt_keys.end(); ++opt_key) {
        if (*opt_key == "perimeters"
            || *opt_key == "extra_perimeters"
            || *opt_key == "gap_fill_speed"
            || *opt_key == "overhangs"
            || *opt_key == "first_layer_extrusion_width"
            || *opt_key == "perimeter_extrusion_width"
            || *opt_key == "infill_overlap"
            || *opt_key == "thin_walls"
            || *opt_key == "external_perimeters_first") {
            steps.insert(posPerimeters);
        } else if (*opt_key == "layer_height"
            || *opt_key == "first_layer_height"
            || *opt_key == "raft_layers") {
            steps.insert(posSlice);
			this->reset_layer_height_profile();
		}
		else if (*opt_key == "clip_multipart_objects" 
            || *opt_key == "xy_size_compensation") {
            steps.insert(posSlice);
        } else if (*opt_key == "support_material"
            || *opt_key == "support_material_angle"
            || *opt_key == "support_material_extruder"
            || *opt_key == "support_material_extrusion_width"
            || *opt_key == "support_material_interface_layers"
            || *opt_key == "support_material_interface_contact_loops"
            || *opt_key == "support_material_interface_extruder"
            || *opt_key == "support_material_interface_spacing"
            || *opt_key == "support_material_interface_speed"
            || *opt_key == "support_material_buildplate_only"
            || *opt_key == "support_material_pattern"
            || *opt_key == "support_material_xy_spacing"
            || *opt_key == "support_material_spacing"
            || *opt_key == "support_material_synchronize_layers"
            || *opt_key == "support_material_threshold"
            || *opt_key == "support_material_with_sheath"
            || *opt_key == "dont_support_bridges"
            || *opt_key == "first_layer_extrusion_width") {
            steps.insert(posSupportMaterial);
        } else if (*opt_key == "interface_shells"
            || *opt_key == "infill_only_where_needed"
            || *opt_key == "infill_every_layers"
            || *opt_key == "solid_infill_every_layers"
            || *opt_key == "bottom_solid_layers"
            || *opt_key == "top_solid_layers"
            || *opt_key == "solid_infill_below_area"
            || *opt_key == "infill_extruder"
            || *opt_key == "solid_infill_extruder"
            || *opt_key == "infill_extrusion_width"
            || *opt_key == "ensure_vertical_shell_thickness") {
            steps.insert(posPrepareInfill);
        } else if (*opt_key == "external_fill_pattern"
            || *opt_key == "external_fill_link_max_length"
            || *opt_key == "fill_angle"
            || *opt_key == "fill_pattern"
            || *opt_key == "fill_link_max_length"
            || *opt_key == "top_infill_extrusion_width"
            || *opt_key == "first_layer_extrusion_width") {
            steps.insert(posInfill);
        } else if (*opt_key == "fill_density"
            || *opt_key == "solid_infill_extrusion_width") {
            steps.insert(posPerimeters);
            steps.insert(posPrepareInfill);
        } else if (*opt_key == "external_perimeter_extrusion_width"
            || *opt_key == "perimeter_extruder") {
            steps.insert(posPerimeters);
            steps.insert(posSupportMaterial);
        } else if (*opt_key == "bridge_flow_ratio") {
            steps.insert(posPerimeters);
            steps.insert(posInfill);
        } else if (*opt_key == "seam_position"
            || *opt_key == "seam_preferred_direction"
            || *opt_key == "seam_preferred_direction_jitter"
            || *opt_key == "support_material_speed"
            || *opt_key == "bridge_speed"
            || *opt_key == "external_perimeter_speed"
            || *opt_key == "infill_speed"
            || *opt_key == "perimeter_speed"
            || *opt_key == "small_perimeter_speed"
            || *opt_key == "solid_infill_speed"
            || *opt_key == "top_solid_infill_speed") {
            // these options only affect G-code export, so nothing to invalidate
        } else {
            // for legacy, if we can't handle this option let's invalidate all steps
			this->reset_layer_height_profile();
			return this->invalidate_all_steps();
        }
    }
    
    bool invalidated = false;
    for (std::set<PrintObjectStep>::const_iterator step = steps.begin(); step != steps.end(); ++step) {
        if (this->invalidate_step(*step)) invalidated = true;
    }
    
    return invalidated;
}

bool
PrintObject::invalidate_step(PrintObjectStep step)
{
    bool invalidated = this->state.invalidate(step);
    
    // propagate to dependent steps
    if (step == posPerimeters) {
        this->invalidate_step(posPrepareInfill);
        this->_print->invalidate_step(psSkirt);
        this->_print->invalidate_step(psBrim);
    } else if (step == posPrepareInfill) {
        this->invalidate_step(posInfill);
    } else if (step == posInfill) {
        this->_print->invalidate_step(psSkirt);
        this->_print->invalidate_step(psBrim);
    } else if (step == posSlice) {
        this->invalidate_step(posPerimeters);
        this->invalidate_step(posSupportMaterial);
    } else if (step == posSupportMaterial) {
        this->_print->invalidate_step(psSkirt);
        this->_print->invalidate_step(psBrim);
    }
    
    return invalidated;
}

bool
PrintObject::invalidate_all_steps()
{
    // make a copy because when invalidating steps the iterators are not working anymore
    std::set<PrintObjectStep> steps = this->state.started;
    
    bool invalidated = false;
    for (std::set<PrintObjectStep>::const_iterator step = steps.begin(); step != steps.end(); ++step) {
        if (this->invalidate_step(*step)) invalidated = true;
    }
    return invalidated;
}

bool
PrintObject::has_support_material() const
{
    return this->config.support_material
        || this->config.raft_layers > 0
        || this->config.support_material_enforce_layers > 0;
}

// This function analyzes slices of a region (SurfaceCollection slices).
// Each region slice (instance of Surface) is analyzed, whether it is supported or whether it is the top surface.
// Initially all slices are of type S_TYPE_INTERNAL.
// Slices are compared against the top / bottom slices and regions and classified to the following groups:
// S_TYPE_TOP - Part of a region, which is not covered by any upper layer. This surface will be filled with a top solid infill.
// S_TYPE_BOTTOMBRIDGE - Part of a region, which is not fully supported, but it hangs in the air, or it hangs losely on a support or a raft.
// S_TYPE_BOTTOM - Part of a region, which is not supported by the same region, but it is supported either by another region, or by a soluble interface layer.
// S_TYPE_INTERNAL - Part of a region, which is supported by the same region type.
// If a part of a region is of S_TYPE_BOTTOM and S_TYPE_TOP, the S_TYPE_BOTTOM wins.
void PrintObject::detect_surfaces_type()
{
    BOOST_LOG_TRIVIAL(info) << "Detecting solid surfaces...";

    // Interface shells: the intersecting parts are treated as self standing objects supporting each other.
    // Each of the objects will have a full number of top / bottom layers, even if these top / bottom layers
    // are completely hidden inside a collective body of intersecting parts.
    // This is useful if one of the parts is to be dissolved, or if it is transparent and the internal shells
    // should be visible.
    bool interface_shells = this->config.interface_shells.value;

    for (int idx_region = 0; idx_region < this->_print->regions.size(); ++ idx_region) {
        BOOST_LOG_TRIVIAL(debug) << "Detecting solid surfaces for region " << idx_region << " in parallel - start";
#ifdef SLIC3R_DEBUG_SLICE_PROCESSING
        for (Layer *layer : this->layers)
            layer->regions[idx_region]->export_region_fill_surfaces_to_svg_debug("1_detect_surfaces_type-initial");
#endif /* SLIC3R_DEBUG_SLICE_PROCESSING */

        // If interface shells are allowed, the region->surfaces cannot be overwritten as they may be used by other threads.
        // Cache the result of the following parallel_loop.
        std::vector<Surfaces> surfaces_new;
        if (interface_shells)
            surfaces_new.assign(this->layers.size(), Surfaces());

        tbb::parallel_for(
            tbb::blocked_range<size_t>(0, this->layers.size()),
            [this, idx_region, interface_shells, &surfaces_new](const tbb::blocked_range<size_t>& range) {
                // If we have raft layers, consider bottom layer as a bridge just like any other bottom surface lying on the void.
                SurfaceType surface_type_bottom_1st =
                    (this->config.raft_layers.value > 0 && this->config.support_material_contact_distance.value > 0) ?
                    stBottomBridge : stBottom;
                // If we have soluble support material, don't bridge. The overhang will be squished against a soluble layer separating
                // the support from the print.
                SurfaceType surface_type_bottom_other =
                    (this->config.support_material.value && this->config.support_material_contact_distance.value == 0) ?
                    stBottom : stBottomBridge;
                for (size_t idx_layer = range.begin(); idx_layer < range.end(); ++ idx_layer) {
                    // BOOST_LOG_TRIVIAL(trace) << "Detecting solid surfaces for region " << idx_region << " and layer " << layer->print_z;
                    Layer       *layer  = this->layers[idx_layer];
                    LayerRegion *layerm = layer->get_region(idx_region);
                    // comparison happens against the *full* slices (considering all regions)
                    // unless internal shells are requested
                    Layer       *upper_layer = idx_layer + 1 < this->layer_count() ? this->get_layer(idx_layer + 1) : nullptr;
                    Layer       *lower_layer = idx_layer > 0 ? this->get_layer(idx_layer - 1) : nullptr;
                    // collapse very narrow parts (using the safety offset in the diff is not enough)
                    float        offset = layerm->flow(frExternalPerimeter).scaled_width() / 10.f;

                    Polygons     layerm_slices_surfaces = to_polygons(layerm->slices.surfaces);

                    // find top surfaces (difference between current surfaces
                    // of current layer and upper one)
                    Surfaces top;
                    if (upper_layer) {
                        Polygons upper_slices = interface_shells ? 
                            to_polygons(upper_layer->get_region(idx_region)->slices.surfaces) : 
                            to_polygons(upper_layer->slices);
                        surfaces_append(top,
                            offset2_ex(diff(layerm_slices_surfaces, upper_slices, true), -offset, offset),
                            stTop);
                    } else {
                        // if no upper layer, all surfaces of this one are solid
                        // we clone surfaces because we're going to clear the slices collection
                        top = layerm->slices.surfaces;
                        for (Surface &surface : top)
                            surface.surface_type = stTop;
                    }
                    
                    // Find bottom surfaces (difference between current surfaces of current layer and lower one).
                    Surfaces bottom;
                    if (lower_layer) {
#if 0
                        //FIXME Why is this branch failing t\multi.t ?
                        Polygons lower_slices = interface_shells ? 
                            to_polygons(lower_layer->get_region(idx_region)->slices.surfaces) : 
                            to_polygons(lower_layer->slices);
                        surfaces_append(bottom,
                            offset2_ex(diff(layerm_slices_surfaces, lower_slices, true), -offset, offset),
                            surface_type_bottom_other);
#else
                        // Any surface lying on the void is a true bottom bridge (an overhang)
                        surfaces_append(
                            bottom,
                            offset2_ex(
                                diff(layerm_slices_surfaces, to_polygons(lower_layer->slices), true), 
                                -offset, offset),
                            surface_type_bottom_other);
                        // if user requested internal shells, we need to identify surfaces
                        // lying on other slices not belonging to this region
                        if (interface_shells) {
                            // non-bridging bottom surfaces: any part of this layer lying 
                            // on something else, excluding those lying on our own region
                            surfaces_append(
                                bottom,
                                offset2_ex(
                                    diff(
                                        intersection(layerm_slices_surfaces, to_polygons(lower_layer->slices)), // supported
                                        to_polygons(lower_layer->get_region(idx_region)->slices.surfaces), 
                                        true), 
                                    -offset, offset),
                                stBottom);
                        }
#endif
                    } else {
                        // if no lower layer, all surfaces of this one are solid
                        // we clone surfaces because we're going to clear the slices collection
                        bottom = layerm->slices.surfaces;
                        for (Surface &surface : bottom)
                            surface.surface_type = surface_type_bottom_1st;
                    }
                    
                    // now, if the object contained a thin membrane, we could have overlapping bottom
                    // and top surfaces; let's do an intersection to discover them and consider them
                    // as bottom surfaces (to allow for bridge detection)
                    if (! top.empty() && ! bottom.empty()) {
        //                Polygons overlapping = intersection(to_polygons(top), to_polygons(bottom));
        //                Slic3r::debugf "  layer %d contains %d membrane(s)\n", $layerm->layer->id, scalar(@$overlapping)
        //                    if $Slic3r::debug;
                        Polygons top_polygons = to_polygons(std::move(top));
                        top.clear();
                        surfaces_append(top,
                            diff_ex(top_polygons, to_polygons(bottom), false),
                            stTop);
                    }

        #ifdef SLIC3R_DEBUG_SLICE_PROCESSING
                    {
                        static int iRun = 0;
                        std::vector<std::pair<Slic3r::ExPolygons, SVG::ExPolygonAttributes>> expolygons_with_attributes;
                        expolygons_with_attributes.emplace_back(std::make_pair(union_ex(top),                           SVG::ExPolygonAttributes("green")));
                        expolygons_with_attributes.emplace_back(std::make_pair(union_ex(bottom),                        SVG::ExPolygonAttributes("brown")));
                        expolygons_with_attributes.emplace_back(std::make_pair(to_expolygons(layerm->slices.surfaces),  SVG::ExPolygonAttributes("black")));
                        SVG::export_expolygons(debug_out_path("1_detect_surfaces_type_%d_region%d-layer_%f.svg", iRun ++, idx_region, layer->print_z).c_str(), expolygons_with_attributes);
                    }
        #endif /* SLIC3R_DEBUG_SLICE_PROCESSING */
                    
                    // save surfaces to layer
                    Surfaces &surfaces_out = interface_shells ? surfaces_new[idx_layer] : layerm->slices.surfaces;
                    surfaces_out.clear();

                    // find internal surfaces (difference between top/bottom surfaces and others)
                    {
                        Polygons topbottom = to_polygons(top);
                        polygons_append(topbottom, to_polygons(bottom));
                        surfaces_append(surfaces_out,
                            diff_ex(layerm_slices_surfaces, topbottom, false),
                            stInternal);
                    }

                    surfaces_append(surfaces_out, std::move(top));
                    surfaces_append(surfaces_out, std::move(bottom));
                    
        //            Slic3r::debugf "  layer %d has %d bottom, %d top and %d internal surfaces\n",
        //                $layerm->layer->id, scalar(@bottom), scalar(@top), scalar(@internal) if $Slic3r::debug;

        #ifdef SLIC3R_DEBUG_SLICE_PROCESSING
                    layerm->export_region_slices_to_svg_debug("detect_surfaces_type-final");
        #endif /* SLIC3R_DEBUG_SLICE_PROCESSING */
                }
            }
        ); // for each layer of a region

        if (interface_shells) {
            // Move surfaces_new to layerm->slices.surfaces
            for (size_t idx_layer = 0; idx_layer < this->layers.size(); ++ idx_layer)
                this->layers[idx_layer]->get_region(idx_region)->slices.surfaces = std::move(surfaces_new[idx_layer]);
        }

        BOOST_LOG_TRIVIAL(debug) << "Detecting solid surfaces for region " << idx_region << " - clipping in parallel - start";
        // Fill in layerm->fill_surfaces by trimming the layerm->slices by the cummulative layerm->fill_surfaces.
        tbb::parallel_for(
            tbb::blocked_range<size_t>(0, this->layers.size()),
            [this, idx_region, interface_shells, &surfaces_new](const tbb::blocked_range<size_t>& range) {
                for (size_t idx_layer = range.begin(); idx_layer < range.end(); ++ idx_layer) {
                    LayerRegion *layerm = this->layers[idx_layer]->get_region(idx_region);
                    layerm->slices_to_fill_surfaces_clipped();
#ifdef SLIC3R_DEBUG_SLICE_PROCESSING
                    layerm->export_region_fill_surfaces_to_svg_debug("1_detect_surfaces_type-final");
#endif /* SLIC3R_DEBUG_SLICE_PROCESSING */
                } // for each layer of a region
            });
        BOOST_LOG_TRIVIAL(debug) << "Detecting solid surfaces for region " << idx_region << " - clipping in parallel - end";
    } // for each $self->print->region_count
}

void
PrintObject::process_external_surfaces()
{
    BOOST_LOG_TRIVIAL(info) << "Processing external surfaces...";

    FOREACH_REGION(this->_print, region) {
        int region_id = int(region - this->_print->regions.begin());
        
        BOOST_LOG_TRIVIAL(debug) << "Processing external surfaces for region " << region_id << " in parallel - start";
        tbb::parallel_for(
            tbb::blocked_range<size_t>(0, this->layers.size()),
            [this, region_id](const tbb::blocked_range<size_t>& range) {
                for (size_t layer_idx = range.begin(); layer_idx < range.end(); ++ layer_idx) {
                    // BOOST_LOG_TRIVIAL(trace) << "Processing external surface, layer" << this->layers[layer_idx]->print_z;
                    this->layers[layer_idx]->get_region(region_id)->process_external_surfaces((layer_idx == 0) ? NULL : this->layers[layer_idx - 1]);
                }
            }
        );
        BOOST_LOG_TRIVIAL(debug) << "Processing external surfaces for region " << region_id << " in parallel - end";
    }
}

struct DiscoverVerticalShellsCacheEntry
{
    // Collected polygons, offsetted
    Polygons    top_slices;
    Polygons    top_fill_surfaces;
    Polygons    bottom_slices;
    Polygons    bottom_fill_surfaces;
};

void
PrintObject::discover_vertical_shells()
{
    PROFILE_FUNC();

    BOOST_LOG_TRIVIAL(info) << "Discovering vertical shells...";

    for (size_t idx_region = 0; idx_region < this->_print->regions.size(); ++ idx_region) {
        PROFILE_BLOCK(discover_vertical_shells_region);

        const PrintRegion &region = *this->_print->get_region(idx_region);
        if (! region.config.ensure_vertical_shell_thickness.value)
            // This region will be handled by discover_horizontal_shells().
            continue;
        int n_extra_top_layers    = std::max(0, region.config.top_solid_layers.value - 1);
        int n_extra_bottom_layers = std::max(0, region.config.bottom_solid_layers.value - 1);
        if (n_extra_top_layers + n_extra_bottom_layers == 0)
            // Zero or 1 layer, there is no additional vertical wall thickness enforced.
            continue;

        BOOST_LOG_TRIVIAL(debug) << "Discovering vertical shells for region " << idx_region << " in parallel - start : cache top / bottom";
        //FIXME Improve the heuristics for a grain size.
        size_t grain_size = std::max(this->layers.size() / 16, size_t(1));
        std::vector<DiscoverVerticalShellsCacheEntry> cache_top_botom_regions(this->layers.size(), DiscoverVerticalShellsCacheEntry());
        tbb::parallel_for(
            tbb::blocked_range<size_t>(0, this->layers.size(), grain_size),
            [this, idx_region, &cache_top_botom_regions](const tbb::blocked_range<size_t>& range) {
                for (size_t idx_layer = range.begin(); idx_layer < range.end(); ++ idx_layer) {
                    LayerRegion &layerm                       = *this->layers[idx_layer]->regions[idx_region];
                    float        min_perimeter_infill_spacing = float(layerm.flow(frSolidInfill).scaled_spacing()) * 1.05f;
                    // Top surfaces.
                    auto &cache = cache_top_botom_regions[idx_layer];
                    cache.top_slices = offset(to_expolygons(layerm.slices.filter_by_type(stTop)), min_perimeter_infill_spacing);
                    cache.top_fill_surfaces = offset(to_expolygons(layerm.fill_surfaces.filter_by_type(stTop)), min_perimeter_infill_spacing);
                    // Bottom surfaces.
                    const SurfaceType surfaces_bottom[2] = { stBottom, stBottomBridge };
                    cache.bottom_slices = offset(to_expolygons(layerm.slices.filter_by_types(surfaces_bottom, 2)), min_perimeter_infill_spacing);
                    cache.bottom_fill_surfaces = offset(to_expolygons(layerm.fill_surfaces.filter_by_types(surfaces_bottom, 2)), min_perimeter_infill_spacing);
                }
            });
        BOOST_LOG_TRIVIAL(debug) << "Discovering vertical shells for region " << idx_region << " in parallel - start : ensure vertical wall thickness";
        tbb::parallel_for(
            tbb::blocked_range<size_t>(0, this->layers.size(), grain_size),
            [this, idx_region, n_extra_top_layers, n_extra_bottom_layers, &cache_top_botom_regions]
            (const tbb::blocked_range<size_t>& range) {
                // printf("discover_vertical_shells from %d to %d\n", range.begin(), range.end());
                for (size_t idx_layer = range.begin(); idx_layer < range.end(); ++ idx_layer) {
                    PROFILE_BLOCK(discover_vertical_shells_region_layer);

#ifdef SLIC3R_DEBUG_SLICE_PROCESSING
        			static size_t debug_idx = 0;
        			++ debug_idx;
#endif /* SLIC3R_DEBUG_SLICE_PROCESSING */

                    Layer       *layer               = this->layers[idx_layer];
                    LayerRegion *layerm              = layer->regions[idx_region];

#ifdef SLIC3R_DEBUG_SLICE_PROCESSING
                    layerm->export_region_slices_to_svg_debug("4_discover_vertical_shells-initial");
                    layerm->export_region_fill_surfaces_to_svg_debug("4_discover_vertical_shells-initial");
#endif /* SLIC3R_DEBUG_SLICE_PROCESSING */

                    Flow         solid_infill_flow   = layerm->flow(frSolidInfill);
                    coord_t      infill_line_spacing = solid_infill_flow.scaled_spacing(); 
                    // Find a union of perimeters below / above this surface to guarantee a minimum shell thickness.
                    Polygons shell;
                    Polygons holes;
#ifdef SLIC3R_DEBUG_SLICE_PROCESSING
                    ExPolygons shell_ex;
#endif /* SLIC3R_DEBUG_SLICE_PROCESSING */
                    float min_perimeter_infill_spacing = float(infill_line_spacing) * 1.05f;
                    if (1)
                    {
                        PROFILE_BLOCK(discover_vertical_shells_region_layer_collect);
#if 0
// #ifdef SLIC3R_DEBUG_SLICE_PROCESSING
                        {
        					Slic3r::SVG svg_cummulative(debug_out_path("discover_vertical_shells-perimeters-before-union-run%d.svg", debug_idx), this->bounding_box());
                            for (int n = (int)idx_layer - n_extra_bottom_layers; n <= (int)idx_layer + n_extra_top_layers; ++ n) {
                                if (n < 0 || n >= (int)this->layers.size())
                                    continue;
                                ExPolygons &expolys = this->layers[n]->perimeter_expolygons;
                                for (size_t i = 0; i < expolys.size(); ++ i) {
        							Slic3r::SVG svg(debug_out_path("discover_vertical_shells-perimeters-before-union-run%d-layer%d-expoly%d.svg", debug_idx, n, i), get_extents(expolys[i]));
                                    svg.draw(expolys[i]);
                                    svg.draw_outline(expolys[i].contour, "black", scale_(0.05));
                                    svg.draw_outline(expolys[i].holes, "blue", scale_(0.05));
                                    svg.Close();

                                    svg_cummulative.draw(expolys[i]);
                                    svg_cummulative.draw_outline(expolys[i].contour, "black", scale_(0.05));
                                    svg_cummulative.draw_outline(expolys[i].holes, "blue", scale_(0.05));
                                }
                            }
                        }
#endif /* SLIC3R_DEBUG_SLICE_PROCESSING */
                        // Reset the top / bottom inflated regions caches of entries, which are out of the moving window.
                        bool hole_first = true;
                        for (int n = (int)idx_layer - n_extra_bottom_layers; n <= (int)idx_layer + n_extra_top_layers; ++ n)
                            if (n >= 0 && n < (int)this->layers.size()) {
                                Layer       &neighbor_layer = *this->layers[n];
                                LayerRegion &neighbor_region = *neighbor_layer.get_region(int(idx_region));
                                Polygons newholes;
                                for (size_t idx_region = 0; idx_region < this->_print->regions.size(); ++ idx_region)
                                    polygons_append(newholes, to_polygons(neighbor_layer.regions[idx_region]->fill_expolygons));
                                if (hole_first) {
                                    hole_first = false;
                                    polygons_append(holes, std::move(newholes));
                                }
                                else if (! holes.empty()) {
                                    holes = intersection(holes, newholes);
                                }
                                size_t n_shell_old = shell.size();
                                const DiscoverVerticalShellsCacheEntry &cache = cache_top_botom_regions[n];
                                if (n > int(idx_layer)) {
                                    // Collect top surfaces.
                                    polygons_append(shell, cache.top_slices);
                                    polygons_append(shell, cache.top_fill_surfaces);
                                }
                                else if (n < int(idx_layer)) {
                                    // Collect bottom and bottom bridge surfaces.
                                    polygons_append(shell, cache.bottom_slices);
                                    polygons_append(shell, cache.bottom_fill_surfaces);
                                }
                                // Running the union_ using the Clipper library piece by piece is cheaper 
                                // than running the union_ all at once.
                                if (n_shell_old < shell.size())
                                   shell = union_(shell, false);
                            }
#ifdef SLIC3R_DEBUG_SLICE_PROCESSING
                        {
        					Slic3r::SVG svg(debug_out_path("discover_vertical_shells-perimeters-before-union-%d.svg", debug_idx), get_extents(shell));
                            svg.draw(shell);
                            svg.draw_outline(shell, "black", scale_(0.05));
                            svg.Close(); 
                        }
#endif /* SLIC3R_DEBUG_SLICE_PROCESSING */
#if 0
                        {
                            PROFILE_BLOCK(discover_vertical_shells_region_layer_shell_);
        //                    shell = union_(shell, true);
                            shell = union_(shell, false); 
                        }
#endif
#ifdef SLIC3R_DEBUG_SLICE_PROCESSING
                        shell_ex = union_ex(shell, true);
#endif /* SLIC3R_DEBUG_SLICE_PROCESSING */
                    }

                    //if (shell.empty())
                    //    continue;

#ifdef SLIC3R_DEBUG_SLICE_PROCESSING
                    {
                        Slic3r::SVG svg(debug_out_path("discover_vertical_shells-perimeters-after-union-%d.svg", debug_idx), get_extents(shell));
                        svg.draw(shell_ex);
                        svg.draw_outline(shell_ex, "black", "blue", scale_(0.05));
                        svg.Close();  
                    }
#endif /* SLIC3R_DEBUG_SLICE_PROCESSING */

#ifdef SLIC3R_DEBUG_SLICE_PROCESSING
                    {
                        Slic3r::SVG svg(debug_out_path("discover_vertical_shells-internal-wshell-%d.svg", debug_idx), get_extents(shell));
                        svg.draw(layerm->fill_surfaces.filter_by_type(stInternal), "yellow", 0.5);
                        svg.draw_outline(layerm->fill_surfaces.filter_by_type(stInternal), "black", "blue", scale_(0.05));
                        svg.draw(shell_ex, "blue", 0.5);
                        svg.draw_outline(shell_ex, "black", "blue", scale_(0.05));
                        svg.Close();
                    } 
                    {
                        Slic3r::SVG svg(debug_out_path("discover_vertical_shells-internalvoid-wshell-%d.svg", debug_idx), get_extents(shell));
                        svg.draw(layerm->fill_surfaces.filter_by_type(stInternalVoid), "yellow", 0.5);
                        svg.draw_outline(layerm->fill_surfaces.filter_by_type(stInternalVoid), "black", "blue", scale_(0.05));
                        svg.draw(shell_ex, "blue", 0.5);
                        svg.draw_outline(shell_ex, "black", "blue", scale_(0.05));
                        svg.Close();
                    } 
                    {
                        Slic3r::SVG svg(debug_out_path("discover_vertical_shells-internalvoid-wshell-%d.svg", debug_idx), get_extents(shell));
                        svg.draw(layerm->fill_surfaces.filter_by_type(stInternalVoid), "yellow", 0.5);
                        svg.draw_outline(layerm->fill_surfaces.filter_by_type(stInternalVoid), "black", "blue", scale_(0.05));
                        svg.draw(shell_ex, "blue", 0.5);
                        svg.draw_outline(shell_ex, "black", "blue", scale_(0.05)); 
                        svg.Close();
                    } 
#endif /* SLIC3R_DEBUG_SLICE_PROCESSING */

                    // Trim the shells region by the internal & internal void surfaces.
                    const SurfaceType surfaceTypesInternal[] = { stInternal, stInternalVoid, stInternalSolid };
                    const Polygons    polygonsInternal = to_polygons(layerm->fill_surfaces.filter_by_types(surfaceTypesInternal, 3));
                    shell = intersection(shell, polygonsInternal, true);
                    polygons_append(shell, diff(polygonsInternal, holes));
                    if (shell.empty())
                        continue;

                    // Append the internal solids, so they will be merged with the new ones.
                    polygons_append(shell, to_polygons(layerm->fill_surfaces.filter_by_type(stInternalSolid)));

                    // These regions will be filled by a rectilinear full infill. Currently this type of infill
                    // only fills regions, which fit at least a single line. To avoid gaps in the sparse infill,
                    // make sure that this region does not contain parts narrower than the infill spacing width.
#ifdef SLIC3R_DEBUG_SLICE_PROCESSING
                    Polygons shell_before = shell;
#endif /* SLIC3R_DEBUG_SLICE_PROCESSING */
#if 1
                    // Intentionally inflate a bit more than how much the region has been shrunk, 
                    // so there will be some overlap between this solid infill and the other infill regions (mainly the sparse infill).
                    shell = offset2(shell, - 0.5f * min_perimeter_infill_spacing, 0.8f * min_perimeter_infill_spacing, ClipperLib::jtSquare);
                    if (shell.empty())
                        continue;
#else
                    // Ensure each region is at least 3x infill line width wide, so it could be filled in.
        //            float margin = float(infill_line_spacing) * 3.f;
                    float margin = float(infill_line_spacing) * 1.5f;
                    // we use a higher miterLimit here to handle areas with acute angles
                    // in those cases, the default miterLimit would cut the corner and we'd
                    // get a triangle in $too_narrow; if we grow it below then the shell
                    // would have a different shape from the external surface and we'd still
                    // have the same angle, so the next shell would be grown even more and so on.
                    Polygons too_narrow = diff(shell, offset2(shell, -margin, margin, ClipperLib::jtMiter, 5.), true);
                    if (! too_narrow.empty()) {
                        // grow the collapsing parts and add the extra area to  the neighbor layer 
                        // as well as to our original surfaces so that we support this 
                        // additional area in the next shell too
                        // make sure our grown surfaces don't exceed the fill area
                        polygons_append(shell, intersection(offset(too_narrow, margin), polygonsInternal));
                    }
#endif
                    ExPolygons new_internal_solid = intersection_ex(polygonsInternal, shell, false);
#ifdef SLIC3R_DEBUG_SLICE_PROCESSING
                    {
                        Slic3r::SVG svg(debug_out_path("discover_vertical_shells-regularized-%d.svg", debug_idx), get_extents(shell_before));
                        // Source shell.
                        svg.draw(union_ex(shell_before, true));
                        // Shell trimmed to the internal surfaces.
                        svg.draw_outline(union_ex(shell, true), "black", "blue", scale_(0.05));
                        // Regularized infill region.
                        svg.draw_outline(new_internal_solid, "red", "magenta", scale_(0.05));
                        svg.Close();  
                    }
#endif /* SLIC3R_DEBUG_SLICE_PROCESSING */

                    // Trim the internal & internalvoid by the shell.
                    Slic3r::ExPolygons new_internal = diff_ex(
                        to_polygons(layerm->fill_surfaces.filter_by_type(stInternal)),
                        shell,
                        false
                    );
                    Slic3r::ExPolygons new_internal_void = diff_ex(
                        to_polygons(layerm->fill_surfaces.filter_by_type(stInternalVoid)),
                        shell,
                        false
                    );

#ifdef SLIC3R_DEBUG_SLICE_PROCESSING
                    {
                        SVG::export_expolygons(debug_out_path("discover_vertical_shells-new_internal-%d.svg", debug_idx), get_extents(shell), new_internal, "black", "blue", scale_(0.05));
        				SVG::export_expolygons(debug_out_path("discover_vertical_shells-new_internal_void-%d.svg", debug_idx), get_extents(shell), new_internal_void, "black", "blue", scale_(0.05));
        				SVG::export_expolygons(debug_out_path("discover_vertical_shells-new_internal_solid-%d.svg", debug_idx), get_extents(shell), new_internal_solid, "black", "blue", scale_(0.05));
                    }
#endif /* SLIC3R_DEBUG_SLICE_PROCESSING */

                    // Assign resulting internal surfaces to layer.
                    const SurfaceType surfaceTypesKeep[] = { stTop, stBottom, stBottomBridge };
                    layerm->fill_surfaces.keep_types(surfaceTypesKeep, sizeof(surfaceTypesKeep)/sizeof(SurfaceType));
                    layerm->fill_surfaces.append(new_internal,       stInternal);
                    layerm->fill_surfaces.append(new_internal_void,  stInternalVoid);
                    layerm->fill_surfaces.append(new_internal_solid, stInternalSolid);
                } // for each layer
            }); 
        BOOST_LOG_TRIVIAL(debug) << "Discovering vertical shells for region " << idx_region << " in parallel - end";

#ifdef SLIC3R_DEBUG_SLICE_PROCESSING
		for (size_t idx_layer = 0; idx_layer < this->layers.size(); ++idx_layer) {
			LayerRegion *layerm = this->layers[idx_layer]->get_region(idx_region);
			layerm->export_region_slices_to_svg_debug("4_discover_vertical_shells-final");
			layerm->export_region_fill_surfaces_to_svg_debug("4_discover_vertical_shells-final");
		}
#endif /* SLIC3R_DEBUG_SLICE_PROCESSING */
    } // for each region

    // Write the profiler measurements to file
//    PROFILE_UPDATE();
//    PROFILE_OUTPUT(debug_out_path("discover_vertical_shells-profile.txt").c_str());
}

/* This method applies bridge flow to the first internal solid layer above
   sparse infill */
void
PrintObject::bridge_over_infill()
{
    BOOST_LOG_TRIVIAL(info) << "Bridge over infill...";

    FOREACH_REGION(this->_print, region) {
        size_t region_id = region - this->_print->regions.begin();
        
        // skip bridging in case there are no voids
        if ((*region)->config.fill_density.value == 100) continue;
        
        // get bridge flow
        Flow bridge_flow = (*region)->flow(
            frSolidInfill,
            -1,     // layer height, not relevant for bridge flow
            true,   // bridge
            false,  // first layer
            -1,     // custom width, not relevant for bridge flow
            *this
        );
        
        FOREACH_LAYER(this, layer_it) {
            // skip first layer
            if (layer_it == this->layers.begin()) continue;
            
            Layer* layer        = *layer_it;
            LayerRegion* layerm = layer->get_region(region_id);
            
            // extract the stInternalSolid surfaces that might be transformed into bridges
            Polygons internal_solid;
            layerm->fill_surfaces.filter_by_type(stInternalSolid, &internal_solid);
            
            // check whether the lower area is deep enough for absorbing the extra flow
            // (for obvious physical reasons but also for preventing the bridge extrudates
            // from overflowing in 3D preview)
            ExPolygons to_bridge;
            {
                Polygons to_bridge_pp = internal_solid;
                
                // iterate through lower layers spanned by bridge_flow
                double bottom_z = layer->print_z - bridge_flow.height;
                for (int i = (layer_it - this->layers.begin()) - 1; i >= 0; --i) {
                    const Layer* lower_layer = this->layers[i];
                    
                    // stop iterating if layer is lower than bottom_z
                    if (lower_layer->print_z < bottom_z) break;
                    
                    // iterate through regions and collect internal surfaces
                    Polygons lower_internal;
                    FOREACH_LAYERREGION(lower_layer, lower_layerm_it)
                        (*lower_layerm_it)->fill_surfaces.filter_by_type(stInternal, &lower_internal);
                    
                    // intersect such lower internal surfaces with the candidate solid surfaces
                    to_bridge_pp = intersection(to_bridge_pp, lower_internal);
                }
                
                // there's no point in bridging too thin/short regions
                //FIXME Vojtech: The offset2 function is not a geometric offset, 
                // therefore it may create 1) gaps, and 2) sharp corners, which are outside the original contour.
                // The gaps will be filled by a separate region, which makes the infill less stable and it takes longer.
                {
                    double min_width = bridge_flow.scaled_width() * 3;
                    to_bridge_pp = offset2(to_bridge_pp, -min_width, +min_width);
                }
                
                if (to_bridge_pp.empty()) continue;
                
                // convert into ExPolygons
                to_bridge = union_ex(to_bridge_pp);
            }
            
            #ifdef SLIC3R_DEBUG
            printf("Bridging " PRINTF_ZU " internal areas at layer " PRINTF_ZU "\n", to_bridge.size(), layer->id());
            #endif
            
            // compute the remaning internal solid surfaces as difference
            ExPolygons not_to_bridge = diff_ex(internal_solid, to_polygons(to_bridge), true);
            to_bridge = intersection_ex(to_polygons(to_bridge), internal_solid, true);
            // build the new collection of fill_surfaces
            layerm->fill_surfaces.remove_type(stInternalSolid);
            for (ExPolygon &ex : to_bridge)
                layerm->fill_surfaces.surfaces.push_back(Surface(stInternalBridge, ex));
            for (ExPolygon &ex : not_to_bridge)
                layerm->fill_surfaces.surfaces.push_back(Surface(stInternalSolid, ex));            
            /*
            # exclude infill from the layers below if needed
            # see discussion at https://github.com/alexrj/Slic3r/issues/240
            # Update: do not exclude any infill. Sparse infill is able to absorb the excess material.
            if (0) {
                my $excess = $layerm->extruders->{infill}->bridge_flow->width - $layerm->height;
                for (my $i = $layer_id-1; $excess >= $self->get_layer($i)->height; $i--) {
                    Slic3r::debugf "  skipping infill below those areas at layer %d\n", $i;
                    foreach my $lower_layerm (@{$self->get_layer($i)->regions}) {
                        my @new_surfaces = ();
                        # subtract the area from all types of surfaces
                        foreach my $group (@{$lower_layerm->fill_surfaces->group}) {
                            push @new_surfaces, map $group->[0]->clone(expolygon => $_),
                                @{diff_ex(
                                    [ map $_->p, @$group ],
                                    [ map @$_, @$to_bridge ],
                                )};
                            push @new_surfaces, map Slic3r::Surface->new(
                                expolygon       => $_,
                                surface_type    => S_TYPE_INTERNALVOID,
                            ), @{intersection_ex(
                                [ map $_->p, @$group ],
                                [ map @$_, @$to_bridge ],
                            )};
                        }
                        $lower_layerm->fill_surfaces->clear;
                        $lower_layerm->fill_surfaces->append($_) for @new_surfaces;
                    }
                    
                    $excess -= $self->get_layer($i)->height;
                }
            }
            */

#ifdef SLIC3R_DEBUG_SLICE_PROCESSING
            layerm->export_region_slices_to_svg_debug("7_bridge_over_infill");
            layerm->export_region_fill_surfaces_to_svg_debug("7_bridge_over_infill");
#endif /* SLIC3R_DEBUG_SLICE_PROCESSING */
        }
    }
}

SlicingParameters PrintObject::slicing_parameters() const
{
    return SlicingParameters::create_from_config(
        this->print()->config, this->config, 
        unscale(this->size.z), this->print()->object_extruders());
}

bool PrintObject::update_layer_height_profile(std::vector<coordf_t> &layer_height_profile) const
{
    bool updated = false;

    // If the layer height profile is not set, try to use the one stored at the ModelObject.
    if (layer_height_profile.empty() && layer_height_profile.data() != this->model_object()->layer_height_profile.data()) {
        layer_height_profile = this->model_object()->layer_height_profile;
        updated = true;
    }

    // Verify the layer_height_profile.
    SlicingParameters slicing_params = this->slicing_parameters();
    if (! layer_height_profile.empty() && 
            // Must not be of even length.
            ((layer_height_profile.size() & 1) != 0 || 
            // Last entry must be at the top of the object.
             std::abs(layer_height_profile[layer_height_profile.size() - 2] - slicing_params.object_print_z_height()) > 1e-3))
        layer_height_profile.clear();

    if (layer_height_profile.empty()) {
        if (0)
//        if (this->layer_height_profile.empty())
            layer_height_profile = layer_height_profile_adaptive(slicing_params, this->layer_height_ranges,
                this->model_object()->volumes);
        else
            layer_height_profile = layer_height_profile_from_ranges(slicing_params, this->layer_height_ranges);
        updated = true;
    }
    return updated;
}

// This must be called from the main thread as it modifies the layer_height_profile.
bool PrintObject::update_layer_height_profile()
{
    // If the layer height profile has been marked as invalid for some reason (modified at the UI level 
    // or invalidated due to the slicing parameters), clear it now.
    if (! this->layer_height_profile_valid) { 
        this->layer_height_profile.clear();
        this->layer_height_profile_valid = true;
    }
    return this->update_layer_height_profile(this->layer_height_profile);
}

// 1) Decides Z positions of the layers,
// 2) Initializes layers and their regions
// 3) Slices the object meshes
// 4) Slices the modifier meshes and reclassifies the slices of the object meshes by the slices of the modifier meshes
// 5) Applies size compensation (offsets the slices in XY plane)
// 6) Replaces bad slices by the slices reconstructed from the upper/lower layer
// Resulting expolygons of layer regions are marked as Internal.
//
// this should be idempotent
void PrintObject::_slice()
{
    BOOST_LOG_TRIVIAL(info) << "Slicing objects...";

#if 0
    // Disable parallelization for debugging purposes.
    static tbb::task_scheduler_init *tbb_init = nullptr;
    tbb_init = new tbb::task_scheduler_init(1);
#endif

    SlicingParameters slicing_params = this->slicing_parameters();

    // 1) Initialize layers and their slice heights.
    std::vector<float> slice_zs;
    {
        this->clear_layers();
        // Object layers (pairs of bottom/top Z coordinate), without the raft.
        this->update_layer_height_profile();
        std::vector<coordf_t> object_layers = generate_object_layers(slicing_params, this->layer_height_profile);
        // Reserve object layers for the raft. Last layer of the raft is the contact layer.
        int id = int(slicing_params.raft_layers());
        slice_zs.reserve(object_layers.size());
        Layer *prev = nullptr;
        for (size_t i_layer = 0; i_layer < object_layers.size(); i_layer += 2) {
            coordf_t lo = object_layers[i_layer];
            coordf_t hi = object_layers[i_layer + 1];
            coordf_t slice_z = 0.5 * (lo + hi);
            Layer *layer = this->add_layer(id ++, hi - lo, hi + slicing_params.object_print_z_min, slice_z);
            slice_zs.push_back(float(slice_z));
            if (prev != nullptr) {
                prev->upper_layer = layer;
                layer->lower_layer = prev;
            }
            // Make sure all layers contain layer region objects for all regions.
            for (size_t region_id = 0; region_id < this->print()->regions.size(); ++ region_id)
                layer->add_region(this->print()->regions[region_id]);
            prev = layer;
        }
    }
    
    // Slice all non-modifier volumes.
    for (size_t region_id = 0; region_id < this->print()->regions.size(); ++ region_id) {
        BOOST_LOG_TRIVIAL(debug) << "Slicing objects - region " << region_id;
        std::vector<ExPolygons> expolygons_by_layer = this->_slice_region(region_id, slice_zs, false);
        BOOST_LOG_TRIVIAL(debug) << "Slicing objects - append slices " << region_id << " start";
        for (size_t layer_id = 0; layer_id < expolygons_by_layer.size(); ++ layer_id)
            this->layers[layer_id]->regions[region_id]->slices.append(std::move(expolygons_by_layer[layer_id]), stInternal);
        BOOST_LOG_TRIVIAL(debug) << "Slicing objects - append slices " << region_id << " end";
    }

    // Slice all modifier volumes.
    if (this->print()->regions.size() > 1) {
        for (size_t region_id = 0; region_id < this->print()->regions.size(); ++ region_id) {
            BOOST_LOG_TRIVIAL(debug) << "Slicing modifier volumes - region " << region_id;
            std::vector<ExPolygons> expolygons_by_layer = this->_slice_region(region_id, slice_zs, true);
            // loop through the other regions and 'steal' the slices belonging to this one
            BOOST_LOG_TRIVIAL(debug) << "Slicing modifier volumes - stealing " << region_id << " start";
            for (size_t other_region_id = 0; other_region_id < this->print()->regions.size(); ++ other_region_id) {
                if (region_id == other_region_id)
                    continue;
                for (size_t layer_id = 0; layer_id < expolygons_by_layer.size(); ++ layer_id) {
                    Layer       *layer = layers[layer_id];
                    LayerRegion *layerm = layer->regions[region_id];
                    LayerRegion *other_layerm = layer->regions[other_region_id];
                    if (layerm == nullptr || other_layerm == nullptr)
                        continue;
                    Polygons other_slices = to_polygons(other_layerm->slices);
                    ExPolygons my_parts = intersection_ex(other_slices, to_polygons(expolygons_by_layer[layer_id]));
                    if (my_parts.empty())
                        continue;
                    // Remove such parts from original region.
                    other_layerm->slices.set(diff_ex(other_slices, to_polygons(my_parts)), stInternal);
                    // Append new parts to our region.
                    layerm->slices.append(std::move(my_parts), stInternal);
                }
            }
            BOOST_LOG_TRIVIAL(debug) << "Slicing modifier volumes - stealing " << region_id << " end";
        }
    }
    
    BOOST_LOG_TRIVIAL(debug) << "Slicing objects - removing top empty layers";
    while (! this->layers.empty()) {
        const Layer *layer = this->layers.back();
        for (size_t region_id = 0; region_id < this->print()->regions.size(); ++ region_id)
            if (layer->regions[region_id] != nullptr && ! layer->regions[region_id]->slices.empty())
                // Non empty layer.
                goto end;
        this->delete_layer(int(this->layers.size()) - 1);
    }
end:
    ;

    BOOST_LOG_TRIVIAL(debug) << "Slicing objects - make_slices in parallel - begin";
    tbb::parallel_for(
        tbb::blocked_range<size_t>(0, this->layers.size()),
        [this](const tbb::blocked_range<size_t>& range) {
            for (size_t layer_id = range.begin(); layer_id < range.end(); ++ layer_id) {
                Layer *layer = this->layers[layer_id];
                // Apply size compensation and perform clipping of multi-part objects.
                float delta = float(scale_(this->config.xy_size_compensation.value));
                bool  scale = delta != 0.f;
                bool  clip  = this->config.clip_multipart_objects.value || delta > 0.f;
                if (layer->regions.size() == 1) {
                    if (scale) {
                        // Single region, growing or shrinking.
                        LayerRegion *layerm = layer->regions.front();
                        layerm->slices.set(offset_ex(to_expolygons(std::move(layerm->slices.surfaces)), delta), stInternal);
                    }
                } else if (scale || clip) {
                    // Multiple regions, growing, shrinking or just clipping one region by the other.
                    // When clipping the regions, priority is given to the first regions.
                    Polygons processed;
        			for (size_t region_id = 0; region_id < layer->regions.size(); ++ region_id) {
                        LayerRegion *layerm = layer->regions[region_id];
        				ExPolygons slices = to_expolygons(std::move(layerm->slices.surfaces));
        				if (scale)
        					slices = offset_ex(slices, delta);
                        if (region_id > 0 && clip) 
                            // Trim by the slices of already processed regions.
                            slices = diff_ex(to_polygons(std::move(slices)), processed);
                        if (clip && region_id + 1 < layer->regions.size())
                            // Collect the already processed regions to trim the to be processed regions.
                            polygons_append(processed, slices);
                        layerm->slices.set(std::move(slices), stInternal);
                    }
                }
                // Merge all regions' slices to get islands, chain them by a shortest path.
                layer->make_slices();
            }
        });
    BOOST_LOG_TRIVIAL(debug) << "Slicing objects - make_slices in parallel - end";
}

std::vector<ExPolygons> PrintObject::_slice_region(size_t region_id, const std::vector<float> &z, bool modifier)
{
    std::vector<ExPolygons> layers;
    assert(region_id < this->region_volumes.size());
    std::vector<int> &volumes = this->region_volumes[region_id];
    if (! volumes.empty()) {
        // Compose mesh.
        //FIXME better to perform slicing over each volume separately and then to use a Boolean operation to merge them.
        TriangleMesh mesh;
        for (std::vector<int>::const_iterator it_volume = volumes.begin(); it_volume != volumes.end(); ++ it_volume) {
            ModelVolume *volume = this->model_object()->volumes[*it_volume];
            if (volume->modifier == modifier)
                mesh.merge(volume->mesh);
        }
        if (mesh.stl.stats.number_of_facets > 0) {
            // transform mesh
            // we ignore the per-instance transformations currently and only 
            // consider the first one
            this->model_object()->instances.front()->transform_mesh(&mesh, true);
            // align mesh to Z = 0 (it should be already aligned actually) and apply XY shift
            mesh.translate(- unscale(this->_copies_shift.x), - unscale(this->_copies_shift.y), -this->model_object()->bounding_box().min.z);
            // perform actual slicing
            TriangleMeshSlicer mslicer(&mesh);
            mslicer.slice(z, &layers);
        }
    }
    return layers;
}

std::string PrintObject::_fix_slicing_errors()
{
    // Collect layers with slicing errors.
    // These layers will be fixed in parallel.
    std::vector<size_t> buggy_layers;
    buggy_layers.reserve(this->layers.size());
    for (size_t idx_layer = 0; idx_layer < this->layers.size(); ++ idx_layer)
        if (this->layers[idx_layer]->slicing_errors)
            buggy_layers.push_back(idx_layer);

    BOOST_LOG_TRIVIAL(debug) << "Slicing objects - fixing slicing errors in parallel - begin";
    tbb::parallel_for(
        tbb::blocked_range<size_t>(0, buggy_layers.size()),
        [this, &buggy_layers](const tbb::blocked_range<size_t>& range) {
            for (size_t buggy_layer_idx = range.begin(); buggy_layer_idx < range.end(); ++ buggy_layer_idx) {
                size_t idx_layer = buggy_layers[buggy_layer_idx];
                Layer *layer     = this->layers[idx_layer];
                assert(layer->slicing_errors);
                // Try to repair the layer surfaces by merging all contours and all holes from neighbor layers.
                // BOOST_LOG_TRIVIAL(trace) << "Attempting to repair layer" << idx_layer;
                for (size_t region_id = 0; region_id < layer->regions.size(); ++ region_id) {
                    LayerRegion *layerm = layer->regions[region_id];
                    // Find the first valid layer below / above the current layer.
                    const Surfaces *upper_surfaces = nullptr;
                    const Surfaces *lower_surfaces = nullptr;
                    for (size_t j = idx_layer + 1; j < this->layers.size(); ++ j)
                        if (! this->layers[j]->slicing_errors) {
                            upper_surfaces = &this->layers[j]->regions[region_id]->slices.surfaces;
                            break;
                        }
                    for (int j = int(idx_layer) - 1; j >= 0; -- j)
                        if (! this->layers[j]->slicing_errors) {
                            lower_surfaces = &this->layers[j]->regions[region_id]->slices.surfaces;
                            break;
                        }
                    // Collect outer contours and holes from the valid layers above & below.
                    Polygons outer;
                    outer.reserve(
                        ((upper_surfaces == nullptr) ? 0 : upper_surfaces->size()) + 
                        ((lower_surfaces == nullptr) ? 0 : lower_surfaces->size()));
                    size_t num_holes = 0;
                    if (upper_surfaces)
                        for (const auto &surface : *upper_surfaces) {
                            outer.push_back(surface.expolygon.contour);
                            num_holes += surface.expolygon.holes.size();
                        }
                    if (lower_surfaces)
                        for (const auto &surface : *lower_surfaces) {
                            outer.push_back(surface.expolygon.contour);
                            num_holes += surface.expolygon.holes.size();
                        }
                    Polygons holes;
                    holes.reserve(num_holes);
                    if (upper_surfaces)
                        for (const auto &surface : *upper_surfaces)
                            polygons_append(holes, surface.expolygon.holes);
                    if (lower_surfaces)
                        for (const auto &surface : *lower_surfaces)
                            polygons_append(holes, surface.expolygon.holes);
                    layerm->slices.set(diff_ex(union_(outer), holes, false), stInternal);
                }
                // Update layer slices after repairing the single regions.
                layer->make_slices();
            }
        });
    BOOST_LOG_TRIVIAL(debug) << "Slicing objects - fixing slicing errors in parallel - end";

    // remove empty layers from bottom
    while (! this->layers.empty() && this->layers.front()->slices.expolygons.empty()) {
        this->delete_layer(0);
        for (size_t i = 0; i < this->layers.size(); ++ i)
            this->layers[i]->set_id(this->layers[i]->id() - 1);
    }

    return buggy_layers.empty() ? "" :
        "The model has overlapping or self-intersecting facets. I tried to repair it, "
        "however you might want to check the results or repair the input file and retry.\n";
}

// Simplify the sliced model, if "resolution" configuration parameter > 0.
// The simplification is problematic, because it simplifies the slices independent from each other,
// which makes the simplified discretization visible on the object surface.
void PrintObject::_simplify_slices(double distance)
{
    BOOST_LOG_TRIVIAL(debug) << "Slicing objects - siplifying slices in parallel - begin";
    tbb::parallel_for(
        tbb::blocked_range<size_t>(0, this->layers.size()),
        [this, distance](const tbb::blocked_range<size_t>& range) {
            for (size_t layer_idx = range.begin(); layer_idx < range.end(); ++ layer_idx) {
                Layer *layer = this->layers[layer_idx];
                for (size_t region_idx = 0; region_idx < layer->regions.size(); ++ region_idx)
                    layer->regions[region_idx]->slices.simplify(distance);
                layer->slices.simplify(distance);
            }
        });
    BOOST_LOG_TRIVIAL(debug) << "Slicing objects - siplifying slices in parallel - end";
}

void
PrintObject::_make_perimeters()
{
    if (this->state.is_done(posPerimeters)) return;
    this->state.set_started(posPerimeters);

    BOOST_LOG_TRIVIAL(info) << "Generating perimeters...";
    
    // merge slices if they were split into types
    if (this->typed_slices) {
        FOREACH_LAYER(this, layer_it)
            (*layer_it)->merge_slices();
        this->typed_slices = false;
        this->state.invalidate(posPrepareInfill);
    }
    
    // compare each layer to the one below, and mark those slices needing
    // one additional inner perimeter, like the top of domed objects-
    
    // this algorithm makes sure that at least one perimeter is overlapping
    // but we don't generate any extra perimeter if fill density is zero, as they would be floating
    // inside the object - infill_only_where_needed should be the method of choice for printing
    // hollow objects
    FOREACH_REGION(this->_print, region_it) {
        size_t region_id = region_it - this->_print->regions.begin();
        const PrintRegion &region = **region_it;
        
        
        if (!region.config.extra_perimeters
            || region.config.perimeters == 0
            || region.config.fill_density == 0
            || this->layer_count() < 2)
            continue;
        
        BOOST_LOG_TRIVIAL(debug) << "Generating extra perimeters for region " << region_id << " in parallel - start";
        tbb::parallel_for(
            tbb::blocked_range<size_t>(0, this->layers.size() - 1),
            [this, &region, region_id](const tbb::blocked_range<size_t>& range) {
                for (size_t layer_idx = range.begin(); layer_idx < range.end(); ++ layer_idx) {
                    LayerRegion &layerm                     = *this->layers[layer_idx]->regions[region_id];
                    const LayerRegion &upper_layerm         = *this->layers[layer_idx+1]->regions[region_id];
                    const Polygons upper_layerm_polygons    = upper_layerm.slices;
                    // Filter upper layer polygons in intersection_ppl by their bounding boxes?
                    // my $upper_layerm_poly_bboxes= [ map $_->bounding_box, @{$upper_layerm_polygons} ];
                    const double total_loop_length      = total_length(upper_layerm_polygons);
                    const coord_t perimeter_spacing     = layerm.flow(frPerimeter).scaled_spacing();
                    const Flow ext_perimeter_flow       = layerm.flow(frExternalPerimeter);
                    const coord_t ext_perimeter_width   = ext_perimeter_flow.scaled_width();
                    const coord_t ext_perimeter_spacing = ext_perimeter_flow.scaled_spacing();

                    for (Surface &slice : layerm.slices.surfaces) {
                        for (;;) {
                            // compute the total thickness of perimeters
                            const coord_t perimeters_thickness = ext_perimeter_width/2 + ext_perimeter_spacing/2
                                + (region.config.perimeters-1 + slice.extra_perimeters) * perimeter_spacing;
                            // define a critical area where we don't want the upper slice to fall into
                            // (it should either lay over our perimeters or outside this area)
                            const coord_t critical_area_depth = coord_t(perimeter_spacing * 1.5);
                            const Polygons critical_area = diff(
                                offset(slice.expolygon, float(- perimeters_thickness)),
                                offset(slice.expolygon, float(- perimeters_thickness - critical_area_depth))
                            );
                            // check whether a portion of the upper slices falls inside the critical area
                            const Polylines intersection = intersection_pl(to_polylines(upper_layerm_polygons), critical_area);
                            // only add an additional loop if at least 30% of the slice loop would benefit from it
                            if (total_length(intersection) <=  total_loop_length*0.3)
                                break;
                            /*
                            if (0) {
                                require "Slic3r/SVG.pm";
                                Slic3r::SVG::output(
                                    "extra.svg",
                                    no_arrows   => 1,
                                    expolygons  => union_ex($critical_area),
                                    polylines   => [ map $_->split_at_first_point, map $_->p, @{$upper_layerm->slices} ],
                                );
                            }
                            */
                            ++ slice.extra_perimeters;
                        }
                        #ifdef DEBUG
                            if (slice.extra_perimeters > 0)
                                printf("  adding %d more perimeter(s) at layer %zu\n", slice.extra_perimeters, layer_idx);
                        #endif
                    }
                }
            });
        BOOST_LOG_TRIVIAL(debug) << "Generating extra perimeters for region " << region_id << " in parallel - end";
    }

    BOOST_LOG_TRIVIAL(debug) << "Generating perimeters in parallel - start";
    tbb::parallel_for(
        tbb::blocked_range<size_t>(0, this->layers.size()),
        [this](const tbb::blocked_range<size_t>& range) {
            for (size_t layer_idx = range.begin(); layer_idx < range.end(); ++ layer_idx)
                this->layers[layer_idx]->make_perimeters();
        }
    );
    BOOST_LOG_TRIVIAL(debug) << "Generating perimeters in parallel - end";

    /*
        simplify slices (both layer and region slices),
        we only need the max resolution for perimeters
    ### This makes this method not-idempotent, so we keep it disabled for now.
    ###$self->_simplify_slices(&Slic3r::SCALED_RESOLUTION);
    */
    
    this->state.set_done(posPerimeters);
}

void
PrintObject::_infill()
{
    if (this->state.is_done(posInfill)) return;
    this->state.set_started(posInfill);
    
    BOOST_LOG_TRIVIAL(debug) << "Filling layers in parallel - start";
    tbb::parallel_for(
        tbb::blocked_range<size_t>(0, this->layers.size()),
        [this](const tbb::blocked_range<size_t>& range) {
            for (size_t layer_idx = range.begin(); layer_idx < range.end(); ++ layer_idx)
                this->layers[layer_idx]->make_fills();
        }
    );
    BOOST_LOG_TRIVIAL(debug) << "Filling layers in parallel - end";

    /*  we could free memory now, but this would make this step not idempotent
    ### $_->fill_surfaces->clear for map @{$_->regions}, @{$object->layers};
    */
    
    this->state.set_done(posInfill);
}

void PrintObject::_generate_support_material()
{
    PrintObjectSupportMaterial support_material(this, PrintObject::slicing_parameters());
    support_material.generate(*this);
}

void PrintObject::reset_layer_height_profile()
{
    // Reset the layer_heigth_profile.
    this->layer_height_profile.clear();
    // Reset the source layer_height_profile if it exists at the ModelObject.
    this->model_object()->layer_height_profile.clear();
    this->model_object()->layer_height_profile_valid = false;
}

} // namespace Slic3r

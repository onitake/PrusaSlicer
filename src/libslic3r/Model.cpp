#include "Model.hpp"
#include "Geometry.hpp"

#include "Format/AMF.hpp"
#include "Format/OBJ.hpp"
#include "Format/PRUS.hpp"
#include "Format/STL.hpp"
#include "Format/3mf.hpp"

#include <float.h>

#include <boost/algorithm/string/predicate.hpp>
#include <boost/algorithm/string/replace.hpp>
#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/iostream.hpp>

#include "SVG.hpp"
#include <Eigen/Dense>

namespace Slic3r {

unsigned int Model::s_auto_extruder_id = 1;

size_t ModelBase::s_last_id = 0;

// Unique object / instance ID for the wipe tower.
ModelID wipe_tower_object_id()
{
    static ModelBase mine;
    return mine.id();
}

ModelID wipe_tower_instance_id()
{
    static ModelBase mine;
    return mine.id();
}

Model& Model::assign_copy(const Model &rhs)
{
    this->copy_id(rhs);
    // copy materials
    this->clear_materials();
    this->materials = rhs.materials;
    for (std::pair<const t_model_material_id, ModelMaterial*> &m : this->materials) {
        // Copy including the ID and m_model.
        m.second = new ModelMaterial(*m.second);
        m.second->set_model(this);
    }
    // copy objects
    this->clear_objects();
    this->objects.reserve(rhs.objects.size());
	for (const ModelObject *model_object : rhs.objects) {
        // Copy including the ID, leave ID set to invalid (zero).
        auto mo = ModelObject::new_copy(*model_object);
        mo->set_model(this);
		this->objects.emplace_back(mo);
    }
    return *this;
}

Model& Model::assign_copy(Model &&rhs)
{
    this->copy_id(rhs);
	// Move materials, adjust the parent pointer.
    this->clear_materials();
    this->materials = std::move(rhs.materials);
    for (std::pair<const t_model_material_id, ModelMaterial*> &m : this->materials)
        m.second->set_model(this);
    rhs.materials.clear();
    // Move objects, adjust the parent pointer.
    this->clear_objects();
	this->objects = std::move(rhs.objects);
    for (ModelObject *model_object : this->objects)
        model_object->set_model(this);
    rhs.objects.clear();
    return *this;
}

void Model::assign_new_unique_ids_recursive()
{
    this->set_new_unique_id();
    for (std::pair<const t_model_material_id, ModelMaterial*> &m : this->materials)
        m.second->assign_new_unique_ids_recursive();
    for (ModelObject *model_object : this->objects)
        model_object->assign_new_unique_ids_recursive();
}

Model Model::read_from_file(const std::string &input_file, DynamicPrintConfig *config, bool add_default_instances)
{
    Model model;

    DynamicPrintConfig temp_config;
    if (config == nullptr)
        config = &temp_config;

    bool result = false;
    if (boost::algorithm::iends_with(input_file, ".stl"))
        result = load_stl(input_file.c_str(), &model);
    else if (boost::algorithm::iends_with(input_file, ".obj"))
        result = load_obj(input_file.c_str(), &model);
    else if (!boost::algorithm::iends_with(input_file, ".zip.amf") && (boost::algorithm::iends_with(input_file, ".amf") ||
        boost::algorithm::iends_with(input_file, ".amf.xml")))
        result = load_amf(input_file.c_str(), config, &model);
    else if (boost::algorithm::iends_with(input_file, ".3mf"))
        result = load_3mf(input_file.c_str(), config, &model);
    else if (boost::algorithm::iends_with(input_file, ".prusa"))
        result = load_prus(input_file.c_str(), &model);
    else
        throw std::runtime_error("Unknown file format. Input file must have .stl, .obj, .amf(.xml) or .prusa extension.");

    if (! result)
        throw std::runtime_error("Loading of a model file failed.");

    if (model.objects.empty())
        throw std::runtime_error("The supplied file couldn't be read because it's empty");
    
    for (ModelObject *o : model.objects)
        o->input_file = input_file;
    
    if (add_default_instances)
        model.add_default_instances();

    return model;
}

Model Model::read_from_archive(const std::string &input_file, DynamicPrintConfig *config, bool add_default_instances)
{
    Model model;

    bool result = false;
    if (boost::algorithm::iends_with(input_file, ".3mf"))
        result = load_3mf(input_file.c_str(), config, &model);
    else if (boost::algorithm::iends_with(input_file, ".zip.amf"))
        result = load_amf(input_file.c_str(), config, &model);
    else
        throw std::runtime_error("Unknown file format. Input file must have .3mf or .zip.amf extension.");

    if (!result)
        throw std::runtime_error("Loading of a model file failed.");

    if (model.objects.empty())
        throw std::runtime_error("The supplied file couldn't be read because it's empty");

    for (ModelObject *o : model.objects)
    {
        if (boost::algorithm::iends_with(input_file, ".zip.amf"))
        {
            // we remove the .zip part of the extension to avoid it be added to filenames when exporting
            o->input_file = boost::ireplace_last_copy(input_file, ".zip.", ".");
        }
        else
            o->input_file = input_file;
    }

    if (add_default_instances)
        model.add_default_instances();

    return model;
}

void Model::repair()
{
    for (ModelObject *o : this->objects)
        o->repair();
}

ModelObject* Model::add_object()
{
    this->objects.emplace_back(new ModelObject(this));
    return this->objects.back();
}

ModelObject* Model::add_object(const char *name, const char *path, const TriangleMesh &mesh)
{
    ModelObject* new_object = new ModelObject(this);
    this->objects.push_back(new_object);
    new_object->name = name;
    new_object->input_file = path;
    ModelVolume *new_volume = new_object->add_volume(mesh);
    new_volume->name = name;
    new_object->invalidate_bounding_box();
    return new_object;
}

ModelObject* Model::add_object(const char *name, const char *path, TriangleMesh &&mesh)
{
    ModelObject* new_object = new ModelObject(this);
    this->objects.push_back(new_object);
    new_object->name = name;
    new_object->input_file = path;
    ModelVolume *new_volume = new_object->add_volume(std::move(mesh));
    new_volume->name = name;
    new_object->invalidate_bounding_box();
    return new_object;
}

ModelObject* Model::add_object(const ModelObject &other)
{
	ModelObject* new_object = ModelObject::new_clone(other);
    new_object->set_model(this);
    this->objects.push_back(new_object);
    return new_object;
}

void Model::delete_object(size_t idx)
{
    ModelObjectPtrs::iterator i = this->objects.begin() + idx;
    delete *i;
    this->objects.erase(i);
}

bool Model::delete_object(ModelObject* object)
{
    if (object != nullptr) {
        size_t idx = 0;
        for (ModelObject *model_object : objects) {
            if (model_object == object) {
                delete model_object;
                objects.erase(objects.begin() + idx);
                return true;
            }
            ++ idx;
        }
    }
    return false;
}

bool Model::delete_object(ModelID id)
{
    if (id.id != 0) {
        size_t idx = 0;
        for (ModelObject *model_object : objects) {
            if (model_object->id() == id) {
                delete model_object;
                objects.erase(objects.begin() + idx);
                return true;
            }
            ++ idx;
        }
    }
    return false;
}

void Model::clear_objects()
{
    for (ModelObject *o : this->objects)
        delete o;
    this->objects.clear();
}

void Model::delete_material(t_model_material_id material_id)
{
    ModelMaterialMap::iterator i = this->materials.find(material_id);
    if (i != this->materials.end()) {
        delete i->second;
        this->materials.erase(i);
    }
}

void Model::clear_materials()
{
    for (auto &m : this->materials)
        delete m.second;
    this->materials.clear();
}

ModelMaterial* Model::add_material(t_model_material_id material_id)
{
    assert(! material_id.empty());
    ModelMaterial* material = this->get_material(material_id);
    if (material == nullptr)
        material = this->materials[material_id] = new ModelMaterial(this);
    return material;
}

ModelMaterial* Model::add_material(t_model_material_id material_id, const ModelMaterial &other)
{
    assert(! material_id.empty());
    // delete existing material if any
    ModelMaterial* material = this->get_material(material_id);
    delete material;
    // set new material
	material = new ModelMaterial(other);
	material->set_model(this);
    this->materials[material_id] = material;
    return material;
}

// makes sure all objects have at least one instance
bool Model::add_default_instances()
{
    // apply a default position to all objects not having one
    for (ModelObject *o : this->objects)
        if (o->instances.empty())
            o->add_instance();
    return true;
}

// this returns the bounding box of the *transformed* instances
BoundingBoxf3 Model::bounding_box() const
{
    BoundingBoxf3 bb;
    for (ModelObject *o : this->objects)
        bb.merge(o->bounding_box());
    return bb;
}

unsigned int Model::update_print_volume_state(const BoundingBoxf3 &print_volume) 
{
    unsigned int num_printable = 0;
    for (ModelObject *model_object : this->objects)
        num_printable += model_object->check_instances_print_volume_state(print_volume);
    return num_printable;
}

bool Model::center_instances_around_point(const Vec2d &point)
{
    BoundingBoxf3 bb;
    for (ModelObject *o : this->objects)
        for (size_t i = 0; i < o->instances.size(); ++ i)
            bb.merge(o->instance_bounding_box(i, false));

    Vec2d shift2 = point - to_2d(bb.center());
	if (std::abs(shift2(0)) < EPSILON && std::abs(shift2(1)) < EPSILON)
		// No significant shift, don't do anything.
		return false;

	Vec3d shift3 = Vec3d(shift2(0), shift2(1), 0.0);
	for (ModelObject *o : this->objects) {
		for (ModelInstance *i : o->instances)
			i->set_offset(i->get_offset() + shift3);
		o->invalidate_bounding_box();
	}
	return true;
}

// flattens everything to a single mesh
TriangleMesh Model::mesh() const
{
    TriangleMesh mesh;
    for (const ModelObject *o : this->objects)
        mesh.merge(o->mesh());
    return mesh;
}

static bool _arrange(const Pointfs &sizes, coordf_t dist, const BoundingBoxf* bb, Pointfs &out)
{
    if (sizes.empty())
        // return if the list is empty or the following call to BoundingBoxf constructor will lead to a crash
        return true;

    // we supply unscaled data to arrange()
    bool result = Slic3r::Geometry::arrange(
        sizes.size(),               // number of parts
        BoundingBoxf(sizes).max,    // width and height of a single cell
        dist,                       // distance between cells
        bb,                         // bounding box of the area to fill
        out                         // output positions
    );

    if (!result && bb != nullptr) {
        // Try to arrange again ignoring bb
        result = Slic3r::Geometry::arrange(
            sizes.size(),               // number of parts
            BoundingBoxf(sizes).max,    // width and height of a single cell
            dist,                       // distance between cells
            nullptr,                    // bounding box of the area to fill
            out                         // output positions
        );
    }
    
    return result;
}

/*  arrange objects preserving their instance count
    but altering their instance positions */
bool Model::arrange_objects(coordf_t dist, const BoundingBoxf* bb)
{
    // get the (transformed) size of each instance so that we take
    // into account their different transformations when packing
    Pointfs instance_sizes;
    Pointfs instance_centers;
    for (const ModelObject *o : this->objects)
        for (size_t i = 0; i < o->instances.size(); ++ i) {
            // an accurate snug bounding box around the transformed mesh.
            BoundingBoxf3 bbox(o->instance_bounding_box(i, true));
            instance_sizes.emplace_back(to_2d(bbox.size()));
            instance_centers.emplace_back(to_2d(bbox.center()));
        }

    Pointfs positions;
    if (! _arrange(instance_sizes, dist, bb, positions))
        return false;

    size_t idx = 0;
    for (ModelObject *o : this->objects) {
        for (ModelInstance *i : o->instances) {
            Vec2d offset_xy = positions[idx] - instance_centers[idx];
            i->set_offset(Vec3d(offset_xy(0), offset_xy(1), i->get_offset(Z)));
            ++idx;
        }
        o->invalidate_bounding_box();
    }

    return true;
}

// Duplicate the entire model preserving instance relative positions.
void Model::duplicate(size_t copies_num, coordf_t dist, const BoundingBoxf* bb)
{
    Pointfs model_sizes(copies_num-1, to_2d(this->bounding_box().size()));
    Pointfs positions;
    if (! _arrange(model_sizes, dist, bb, positions))
        throw std::invalid_argument("Cannot duplicate part as the resulting objects would not fit on the print bed.\n");
    
    // note that this will leave the object count unaltered
    
    for (ModelObject *o : this->objects) {
        // make a copy of the pointers in order to avoid recursion when appending their copies
        ModelInstancePtrs instances = o->instances;
        for (const ModelInstance *i : instances) {
            for (const Vec2d &pos : positions) {
                ModelInstance *instance = o->add_instance(*i);
                instance->set_offset(instance->get_offset() + Vec3d(pos(0), pos(1), 0.0));
            }
        }
        o->invalidate_bounding_box();
    }
}

/*  this will append more instances to each object
    and then automatically rearrange everything */
void Model::duplicate_objects(size_t copies_num, coordf_t dist, const BoundingBoxf* bb)
{
    for (ModelObject *o : this->objects) {
        // make a copy of the pointers in order to avoid recursion when appending their copies
        ModelInstancePtrs instances = o->instances;
        for (const ModelInstance *i : instances)
            for (size_t k = 2; k <= copies_num; ++ k)
                o->add_instance(*i);
    }
    
    this->arrange_objects(dist, bb);
}

void Model::duplicate_objects_grid(size_t x, size_t y, coordf_t dist)
{
    if (this->objects.size() > 1) throw "Grid duplication is not supported with multiple objects";
    if (this->objects.empty()) throw "No objects!";

    ModelObject* object = this->objects.front();
    object->clear_instances();

    Vec3d ext_size = object->bounding_box().size() + dist * Vec3d::Ones();

    for (size_t x_copy = 1; x_copy <= x; ++x_copy) {
        for (size_t y_copy = 1; y_copy <= y; ++y_copy) {
            ModelInstance* instance = object->add_instance();
            instance->set_offset(Vec3d(ext_size(0) * (double)(x_copy - 1), ext_size(1) * (double)(y_copy - 1), 0.0));
        }
    }
}

bool Model::looks_like_multipart_object() const
{
    if (this->objects.size() <= 1)
        return false;
    double zmin = std::numeric_limits<double>::max();
    for (const ModelObject *obj : this->objects) {
        if (obj->volumes.size() > 1 || obj->config.keys().size() > 1)
            return false;
        for (const ModelVolume *vol : obj->volumes) {
            double zmin_this = vol->mesh.bounding_box().min(2);
            if (zmin == std::numeric_limits<double>::max())
                zmin = zmin_this;
            else if (std::abs(zmin - zmin_this) > EPSILON)
                // The volumes don't share zmin.
                return true;
        }
    }
    return false;
}

void Model::convert_multipart_object(unsigned int max_extruders)
{
    if (this->objects.empty())
        return;
    
    ModelObject* object = new ModelObject(this);
    object->input_file = this->objects.front()->input_file;
    object->name = this->objects.front()->name;
    //FIXME copy the config etc?

    reset_auto_extruder_id();

    for (const ModelObject* o : this->objects)
        for (const ModelVolume* v : o->volumes)
        {
            ModelVolume* new_v = object->add_volume(*v);
            if (new_v != nullptr)
            {
                new_v->name = o->name;
                new_v->config.set_deserialize("extruder", get_auto_extruder_id_as_string(max_extruders));
#if ENABLE_VOLUMES_CENTERING_FIXES
                new_v->translate(-o->origin_translation);
#endif // ENABLE_VOLUMES_CENTERING_FIXES
            }
        }

    for (const ModelInstance* i : this->objects.front()->instances)
        object->add_instance(*i);
    
    this->clear_objects();
    this->objects.push_back(object);
}

void Model::adjust_min_z()
{
    if (objects.empty())
        return;

    if (bounding_box().min(2) < 0.0)
    {
        for (ModelObject* obj : objects)
        {
            if (obj != nullptr)
            {
                coordf_t obj_min_z = obj->bounding_box().min(2);
                if (obj_min_z < 0.0)
                    obj->translate_instances(Vec3d(0.0, 0.0, -obj_min_z));
            }
        }
    }
}

unsigned int Model::get_auto_extruder_id(unsigned int max_extruders)
{
    unsigned int id = s_auto_extruder_id;
    if (id > max_extruders) {
        // The current counter is invalid, likely due to switching the printer profiles
        // to a profile with a lower number of extruders.
        reset_auto_extruder_id();
        id = s_auto_extruder_id;
    } else if (++ s_auto_extruder_id > max_extruders) {
        reset_auto_extruder_id();
    }
    return id;
}

std::string Model::get_auto_extruder_id_as_string(unsigned int max_extruders)
{
    char str_extruder[64];
    sprintf(str_extruder, "%ud", get_auto_extruder_id(max_extruders));
    return str_extruder;
}

void Model::reset_auto_extruder_id()
{
    s_auto_extruder_id = 1;
}

// Propose a filename including path derived from the ModelObject's input path.
// If object's name is filled in, use the object name, otherwise use the input name.
std::string Model::propose_export_file_name_and_path() const
{
    std::string input_file;
    for (const ModelObject *model_object : this->objects)
        for (ModelInstance *model_instance : model_object->instances)
            if (model_instance->is_printable()) {
                input_file = model_object->get_export_filename();

                if (!input_file.empty())
                    goto end;
                // Other instances will produce the same name, skip them.
                break;
            }
end:
    return input_file;
}

std::string Model::propose_export_file_name_and_path(const std::string &new_extension) const
{
    return boost::filesystem::path(this->propose_export_file_name_and_path()).replace_extension(new_extension).string();
}

ModelObject::~ModelObject()
{
    this->clear_volumes();
    this->clear_instances();
}

// maintains the m_model pointer
ModelObject& ModelObject::assign_copy(const ModelObject &rhs)
{
    this->copy_id(rhs);

    this->name                        = rhs.name;
    this->input_file                  = rhs.input_file;
    this->config                      = rhs.config;
    this->sla_support_points          = rhs.sla_support_points;
    this->sla_points_status           = rhs.sla_points_status;
    this->layer_height_ranges         = rhs.layer_height_ranges;
    this->layer_height_profile        = rhs.layer_height_profile;
    this->origin_translation          = rhs.origin_translation;
    m_bounding_box                    = rhs.m_bounding_box;
    m_bounding_box_valid              = rhs.m_bounding_box_valid;
    m_raw_bounding_box                = rhs.m_raw_bounding_box;
    m_raw_bounding_box_valid          = rhs.m_raw_bounding_box_valid;
    m_raw_mesh_bounding_box           = rhs.m_raw_mesh_bounding_box;
    m_raw_mesh_bounding_box_valid     = rhs.m_raw_mesh_bounding_box_valid;

    this->clear_volumes();
    this->volumes.reserve(rhs.volumes.size());
    for (ModelVolume *model_volume : rhs.volumes) {
        this->volumes.emplace_back(new ModelVolume(*model_volume));
        this->volumes.back()->set_model_object(this);
    }
    this->clear_instances();
	this->instances.reserve(rhs.instances.size());
    for (const ModelInstance *model_instance : rhs.instances) {
        this->instances.emplace_back(new ModelInstance(*model_instance));
        this->instances.back()->set_model_object(this);
    }

    return *this;
}

// maintains the m_model pointer
ModelObject& ModelObject::assign_copy(ModelObject &&rhs)
{
    this->copy_id(rhs);

    this->name                        = std::move(rhs.name);
    this->input_file                  = std::move(rhs.input_file);
    this->config                      = std::move(rhs.config);
    this->sla_support_points          = std::move(rhs.sla_support_points);
    this->sla_points_status           = std::move(rhs.sla_points_status);
    this->layer_height_ranges         = std::move(rhs.layer_height_ranges);
    this->layer_height_profile        = std::move(rhs.layer_height_profile);
    this->origin_translation          = std::move(rhs.origin_translation);
    m_bounding_box                    = std::move(rhs.m_bounding_box);
    m_bounding_box_valid              = std::move(rhs.m_bounding_box_valid);
    m_raw_bounding_box                = rhs.m_raw_bounding_box;
    m_raw_bounding_box_valid          = rhs.m_raw_bounding_box_valid;
    m_raw_mesh_bounding_box           = rhs.m_raw_mesh_bounding_box;
    m_raw_mesh_bounding_box_valid     = rhs.m_raw_mesh_bounding_box_valid;

    this->clear_volumes();
	this->volumes = std::move(rhs.volumes);
	rhs.volumes.clear();
    for (ModelVolume *model_volume : this->volumes)
        model_volume->set_model_object(this);
    this->clear_instances();
	this->instances = std::move(rhs.instances);
	rhs.instances.clear();
    for (ModelInstance *model_instance : this->instances)
        model_instance->set_model_object(this);

    return *this;
}

void ModelObject::assign_new_unique_ids_recursive()
{
    this->set_new_unique_id();
    for (ModelVolume *model_volume : this->volumes)
        model_volume->assign_new_unique_ids_recursive();
    for (ModelInstance *model_instance : this->instances)
        model_instance->assign_new_unique_ids_recursive();
}

// Clone this ModelObject including its volumes and instances, keep the IDs of the copies equal to the original.
// Called by Print::apply() to clone the Model / ModelObject hierarchy to the back end for background processing.
//ModelObject* ModelObject::clone(Model *parent)
//{
//    return new ModelObject(parent, *this, true);
//}

ModelVolume* ModelObject::add_volume(const TriangleMesh &mesh)
{
    ModelVolume* v = new ModelVolume(this, mesh);
    this->volumes.push_back(v);
#if ENABLE_VOLUMES_CENTERING_FIXES
    v->center_geometry();
#endif // ENABLE_VOLUMES_CENTERING_FIXES
    this->invalidate_bounding_box();
    return v;
}

ModelVolume* ModelObject::add_volume(TriangleMesh &&mesh)
{
    ModelVolume* v = new ModelVolume(this, std::move(mesh));
    this->volumes.push_back(v);
#if ENABLE_VOLUMES_CENTERING_FIXES
    v->center_geometry();
#endif // ENABLE_VOLUMES_CENTERING_FIXES
    this->invalidate_bounding_box();
    return v;
}

ModelVolume* ModelObject::add_volume(const ModelVolume &other)
{
    ModelVolume* v = new ModelVolume(this, other);
    this->volumes.push_back(v);
#if ENABLE_VOLUMES_CENTERING_FIXES
    v->center_geometry();
#endif // ENABLE_VOLUMES_CENTERING_FIXES
    this->invalidate_bounding_box();
    return v;
}

ModelVolume* ModelObject::add_volume(const ModelVolume &other, TriangleMesh &&mesh)
{
    ModelVolume* v = new ModelVolume(this, other, std::move(mesh));
    this->volumes.push_back(v);
#if ENABLE_VOLUMES_CENTERING_FIXES
    v->center_geometry();
#endif // ENABLE_VOLUMES_CENTERING_FIXES
    this->invalidate_bounding_box();
    return v;
}

void ModelObject::delete_volume(size_t idx)
{
    ModelVolumePtrs::iterator i = this->volumes.begin() + idx;
    delete *i;
    this->volumes.erase(i);

#if ENABLE_VOLUMES_CENTERING_FIXES
    if (this->volumes.size() == 1)
    {
        // only one volume left
        // we need to collapse the volume transform into the instances transforms because now when selecting this volume
        // it will be seen as a single full instance ans so its volume transform may be ignored
        ModelVolume* v = this->volumes.front();
        Transform3d v_t = v->get_transformation().get_matrix();
        for (ModelInstance* inst : this->instances)
        {
            inst->set_transformation(Geometry::Transformation(inst->get_transformation().get_matrix() * v_t));
        }
        Geometry::Transformation t;
        v->set_transformation(t);
        v->set_new_unique_id();
    }
#else
    if (this->volumes.size() == 1)
    {
        // only one volume left
        // center it and update the instances accordingly
        // rationale: the volume may be shifted with respect to the object center and this may lead to wrong rotation and scaling 
        // when modifying the instance matrix of the derived GLVolume
        ModelVolume* v = this->volumes.front();
        v->center_geometry();
        const Vec3d& vol_offset = v->get_offset();
        for (ModelInstance* inst : this->instances)
        {
            inst->set_offset(inst->get_offset() + inst->get_matrix(true) * vol_offset);
        }
        v->set_offset(Vec3d::Zero());
        v->set_new_unique_id();
    }
#endif // ENABLE_VOLUMES_CENTERING_FIXES

    this->invalidate_bounding_box();
}

void ModelObject::clear_volumes()
{
    for (ModelVolume *v : this->volumes)
        delete v;
    this->volumes.clear();
    this->invalidate_bounding_box();
}

ModelInstance* ModelObject::add_instance()
{
    ModelInstance* i = new ModelInstance(this);
    this->instances.push_back(i);
    this->invalidate_bounding_box();
    return i;
}

ModelInstance* ModelObject::add_instance(const ModelInstance &other)
{
    ModelInstance* i = new ModelInstance(this, other);
    this->instances.push_back(i);
    this->invalidate_bounding_box();
    return i;
}

ModelInstance* ModelObject::add_instance(const Vec3d &offset, const Vec3d &scaling_factor, const Vec3d &rotation, const Vec3d &mirror)
{
    auto *instance = add_instance();
    instance->set_offset(offset);
    instance->set_scaling_factor(scaling_factor);
    instance->set_rotation(rotation);
    instance->set_mirror(mirror);
    return instance;
}

void ModelObject::delete_instance(size_t idx)
{
    ModelInstancePtrs::iterator i = this->instances.begin() + idx;
    delete *i;
    this->instances.erase(i);
    this->invalidate_bounding_box();
}

void ModelObject::delete_last_instance()
{
    this->delete_instance(this->instances.size() - 1);
}

void ModelObject::clear_instances()
{
    for (ModelInstance *i : this->instances)
        delete i;
    this->instances.clear();
    this->invalidate_bounding_box();
}

// Returns the bounding box of the transformed instances.
// This bounding box is approximate and not snug.
const BoundingBoxf3& ModelObject::bounding_box() const
{
    if (! m_bounding_box_valid) {
        m_bounding_box_valid = true;
        BoundingBoxf3 raw_bbox = this->raw_mesh_bounding_box();
        m_bounding_box.reset();
        for (const ModelInstance *i : this->instances)
            m_bounding_box.merge(i->transform_bounding_box(raw_bbox));
    }
    return m_bounding_box;
}

// A mesh containing all transformed instances of this object.
TriangleMesh ModelObject::mesh() const
{
    TriangleMesh mesh;
    TriangleMesh raw_mesh = this->raw_mesh();
    for (const ModelInstance *i : this->instances) {
        TriangleMesh m = raw_mesh;
        i->transform_mesh(&m);
        mesh.merge(m);
    }
    return mesh;
}

// Non-transformed (non-rotated, non-scaled, non-translated) sum of non-modifier object volumes.
// Currently used by ModelObject::mesh(), to calculate the 2D envelope for 2D platter
// and to display the object statistics at ModelObject::print_info().
TriangleMesh ModelObject::raw_mesh() const
{
    TriangleMesh mesh;
    for (const ModelVolume *v : this->volumes)
        if (v->is_model_part())
        {
            TriangleMesh vol_mesh(v->mesh);
            vol_mesh.transform(v->get_matrix());
            mesh.merge(vol_mesh);
        }
    return mesh;
}

// Non-transformed (non-rotated, non-scaled, non-translated) sum of all object volumes.
TriangleMesh ModelObject::full_raw_mesh() const
{
    TriangleMesh mesh;
    for (const ModelVolume *v : this->volumes)
    {
        TriangleMesh vol_mesh(v->mesh);
        vol_mesh.transform(v->get_matrix());
        mesh.merge(vol_mesh);
    }
    return mesh;
}

const BoundingBoxf3& ModelObject::raw_mesh_bounding_box() const
{
    if (! m_raw_mesh_bounding_box_valid) {
        m_raw_mesh_bounding_box_valid = true;
        m_raw_mesh_bounding_box.reset();
        for (const ModelVolume *v : this->volumes)
            if (v->is_model_part())
                m_raw_mesh_bounding_box.merge(v->mesh.transformed_bounding_box(v->get_matrix()));
    }
    return m_raw_mesh_bounding_box;
}

BoundingBoxf3 ModelObject::full_raw_mesh_bounding_box() const
{
	BoundingBoxf3 bb;
	for (const ModelVolume *v : this->volumes)
		bb.merge(v->mesh.transformed_bounding_box(v->get_matrix()));
	return bb;
}

// A transformed snug bounding box around the non-modifier object volumes, without the translation applied.
// This bounding box is only used for the actual slicing and for layer editing UI to calculate the layers.
const BoundingBoxf3& ModelObject::raw_bounding_box() const
{
    if (! m_raw_bounding_box_valid) {
        m_raw_bounding_box_valid = true;
        m_raw_bounding_box.reset();
    #if ENABLE_GENERIC_SUBPARTS_PLACEMENT
        if (this->instances.empty())
            throw std::invalid_argument("Can't call raw_bounding_box() with no instances");

        const Transform3d& inst_matrix = this->instances.front()->get_transformation().get_matrix(true);
    #endif // ENABLE_GENERIC_SUBPARTS_PLACEMENT
        for (const ModelVolume *v : this->volumes)
            if (v->is_model_part()) {
    #if !ENABLE_GENERIC_SUBPARTS_PLACEMENT
                if (this->instances.empty())
                    throw std::invalid_argument("Can't call raw_bounding_box() with no instances");
    #endif // !ENABLE_GENERIC_SUBPARTS_PLACEMENT

    #if ENABLE_GENERIC_SUBPARTS_PLACEMENT
				m_raw_bounding_box.merge(v->mesh.transformed_bounding_box(inst_matrix * v->get_matrix()));
    #else
                // unmaintaned
                assert(false);
                // vol_mesh.transform(v->get_matrix());
                // m_raw_bounding_box_valid.merge(this->instances.front()->transform_mesh_bounding_box(vol_mesh, true));
    #endif // ENABLE_GENERIC_SUBPARTS_PLACEMENT
            }
    }
	return m_raw_bounding_box;
}

// This returns an accurate snug bounding box of the transformed object instance, without the translation applied.
BoundingBoxf3 ModelObject::instance_bounding_box(size_t instance_idx, bool dont_translate) const
{
    BoundingBoxf3 bb;
#if ENABLE_GENERIC_SUBPARTS_PLACEMENT
    const Transform3d& inst_matrix = this->instances[instance_idx]->get_transformation().get_matrix(dont_translate);
#endif // ENABLE_GENERIC_SUBPARTS_PLACEMENT
    for (ModelVolume *v : this->volumes)
    {
        if (v->is_model_part())
        {
#if ENABLE_GENERIC_SUBPARTS_PLACEMENT
            bb.merge(v->mesh.transformed_bounding_box(inst_matrix * v->get_matrix()));
#else
            // not maintained
            assert(false);
            //mesh.transform(v->get_matrix());
            //bb.merge(this->instances[instance_idx]->transform_mesh_bounding_box(mesh, dont_translate));
#endif // ENABLE_GENERIC_SUBPARTS_PLACEMENT
        }
    }
    return bb;
}

// Calculate 2D convex hull of of a projection of the transformed printable volumes into the XY plane.
// This method is cheap in that it does not make any unnecessary copy of the volume meshes.
// This method is used by the auto arrange function.
Polygon ModelObject::convex_hull_2d(const Transform3d &trafo_instance) const
{
    Points pts;
    for (const ModelVolume *v : this->volumes)
        if (v->is_model_part()) {
            const stl_file &stl = v->mesh.stl;
            Transform3d trafo = trafo_instance * v->get_matrix();
            if (stl.v_shared == nullptr) {
                // Using the STL faces.
                for (unsigned int i = 0; i < stl.stats.number_of_facets; ++ i) {
                    const stl_facet &facet = stl.facet_start[i];
                    for (size_t j = 0; j < 3; ++ j) {
                        Vec3d p = trafo * facet.vertex[j].cast<double>();
                        pts.emplace_back(coord_t(scale_(p.x())), coord_t(scale_(p.y())));
                    }
                }
            } else {
                // Using the shared vertices should be a bit quicker than using the STL faces.
                for (int i = 0; i < stl.stats.shared_vertices; ++ i) {           
                    Vec3d p = trafo * stl.v_shared[i].cast<double>();
                    pts.emplace_back(coord_t(scale_(p.x())), coord_t(scale_(p.y())));
                }
            }
        }
    std::sort(pts.begin(), pts.end(), [](const Point& a, const Point& b) { return a(0) < b(0) || (a(0) == b(0) && a(1) < b(1)); });
    pts.erase(std::unique(pts.begin(), pts.end(), [](const Point& a, const Point& b) { return a(0) == b(0) && a(1) == b(1); }), pts.end());

    Polygon hull;
    int n = (int)pts.size();
    if (n >= 3) {
        int k = 0;
        hull.points.resize(2 * n);
        // Build lower hull
        for (int i = 0; i < n; ++ i) {
            while (k >= 2 && pts[i].ccw(hull[k-2], hull[k-1]) <= 0)
                -- k;
            hull[k ++] = pts[i];
        }
        // Build upper hull
        for (int i = n-2, t = k+1; i >= 0; i--) {
            while (k >= t && pts[i].ccw(hull[k-2], hull[k-1]) <= 0)
                -- k;
            hull[k ++] = pts[i];
        }
        hull.points.resize(k);
        assert(hull.points.front() == hull.points.back());
        hull.points.pop_back();
    }
    return hull;
}

#if ENABLE_VOLUMES_CENTERING_FIXES
void ModelObject::center_around_origin(bool include_modifiers)
#else
void ModelObject::center_around_origin()
#endif // ENABLE_VOLUMES_CENTERING_FIXES
{
    // calculate the displacements needed to 
    // center this object around the origin
#if ENABLE_VOLUMES_CENTERING_FIXES
    BoundingBoxf3 bb = include_modifiers ? full_raw_mesh_bounding_box() : raw_mesh_bounding_box();
#else
	BoundingBoxf3 bb;
	for (ModelVolume *v : this->volumes)
        if (v->is_model_part())
            bb.merge(v->mesh.bounding_box());
#endif // ENABLE_VOLUMES_CENTERING_FIXES

    // Shift is the vector from the center of the bounding box to the origin
    Vec3d shift = -bb.center();

    this->translate(shift);
    this->origin_translation += shift;
}

void ModelObject::ensure_on_bed()
{
    translate_instances(Vec3d(0.0, 0.0, -get_min_z()));
}

void ModelObject::translate_instances(const Vec3d& vector)
{
    for (size_t i = 0; i < instances.size(); ++i)
    {
        translate_instance(i, vector);
    }
}

void ModelObject::translate_instance(size_t instance_idx, const Vec3d& vector)
{
    ModelInstance* i = instances[instance_idx];
    i->set_offset(i->get_offset() + vector);
    invalidate_bounding_box();
}

void ModelObject::translate(double x, double y, double z)
{
    for (ModelVolume *v : this->volumes)
    {
        v->translate(x, y, z);
    }

    if (m_bounding_box_valid)
        m_bounding_box.translate(x, y, z);
}

void ModelObject::scale(const Vec3d &versor)
{
    for (ModelVolume *v : this->volumes)
    {
        v->scale(versor);
    }
    this->invalidate_bounding_box();
}

void ModelObject::rotate(double angle, Axis axis)
{
    for (ModelVolume *v : this->volumes)
    {
        v->rotate(angle, axis);
    }

    center_around_origin();
    this->invalidate_bounding_box();
}

void ModelObject::rotate(double angle, const Vec3d& axis)
{
    for (ModelVolume *v : this->volumes)
    {
        v->rotate(angle, axis);
    }

    center_around_origin();
    this->invalidate_bounding_box();
}

void ModelObject::mirror(Axis axis)
{
    for (ModelVolume *v : this->volumes)
    {
        v->mirror(axis);
    }

    this->invalidate_bounding_box();
}

void ModelObject::scale_mesh(const Vec3d &versor)
{
    for (ModelVolume *v : this->volumes)
    {
        v->scale_geometry(versor);
        v->set_offset(versor.cwiseProduct(v->get_offset()));
    }
    this->invalidate_bounding_box();
}

size_t ModelObject::materials_count() const
{
    std::set<t_model_material_id> material_ids;
    for (const ModelVolume *v : this->volumes)
        material_ids.insert(v->material_id());
    return material_ids.size();
}

size_t ModelObject::facets_count() const
{
    size_t num = 0;
    for (const ModelVolume *v : this->volumes)
        if (v->is_model_part())
            num += v->mesh.stl.stats.number_of_facets;
    return num;
}

bool ModelObject::needed_repair() const
{
    for (const ModelVolume *v : this->volumes)
        if (v->is_model_part() && v->mesh.needed_repair())
            return true;
    return false;
}

ModelObjectPtrs ModelObject::cut(size_t instance, coordf_t z, bool keep_upper, bool keep_lower, bool rotate_lower)
{
    if (!keep_upper && !keep_lower) { return {}; }

    BOOST_LOG_TRIVIAL(trace) << "ModelObject::cut - start";

    // Clone the object to duplicate instances, materials etc.
    ModelObject* upper = keep_upper ? ModelObject::new_clone(*this) : nullptr;
    ModelObject* lower = keep_lower ? ModelObject::new_clone(*this) : nullptr;

    if (keep_upper) {
        upper->set_model(nullptr);
        upper->sla_support_points.clear();
        upper->sla_points_status = sla::PointsStatus::None;
        upper->clear_volumes();
        upper->input_file = "";
    }

    if (keep_lower) {
        lower->set_model(nullptr);
        lower->sla_support_points.clear();
        lower->sla_points_status = sla::PointsStatus::None;
        lower->clear_volumes();
        lower->input_file = "";
    }

    // Because transformations are going to be applied to meshes directly,
    // we reset transformation of all instances and volumes,
    // except for translation and Z-rotation on instances, which are preserved
    // in the transformation matrix and not applied to the mesh transform.

    // const auto instance_matrix = instances[instance]->get_matrix(true);
    const auto instance_matrix = Geometry::assemble_transform(
        Vec3d::Zero(),  // don't apply offset
        instances[instance]->get_rotation().cwiseProduct(Vec3d(1.0, 1.0, 0.0)),   // don't apply Z-rotation
        instances[instance]->get_scaling_factor(),
        instances[instance]->get_mirror()
    );

    z -= instances[instance]->get_offset()(2);

    // Lower part per-instance bounding boxes
    std::vector<BoundingBoxf3> lower_bboxes { instances.size() };

    for (ModelVolume *volume : volumes) {
        const auto volume_matrix = volume->get_matrix();

        if (! volume->is_model_part()) {
            // Modifiers are not cut, but we still need to add the instance transformation
            // to the modifier volume transformation to preserve their shape properly.

            volume->set_transformation(Geometry::Transformation(instance_matrix * volume_matrix));

            if (keep_upper) { upper->add_volume(*volume); }
            if (keep_lower) { lower->add_volume(*volume); }
        }
        else {
            TriangleMesh upper_mesh, lower_mesh;

            // Transform the mesh by the combined transformation matrix.
            // Flip the triangles in case the composite transformation is left handed.
            volume->mesh.transform(instance_matrix * volume_matrix, true);

            // Perform cut
            volume->mesh.require_shared_vertices(); // TriangleMeshSlicer needs this
            TriangleMeshSlicer tms(&volume->mesh);
            tms.cut(float(z), &upper_mesh, &lower_mesh);

            // Reset volume transformation except for offset
            const Vec3d offset = volume->get_offset();
            volume->set_transformation(Geometry::Transformation());
            volume->set_offset(offset);

            if (keep_upper) {
                upper_mesh.repair();
                upper_mesh.reset_repair_stats();
            }
            if (keep_lower) {
                lower_mesh.repair();
                lower_mesh.reset_repair_stats();
            }

            if (keep_upper && upper_mesh.facets_count() > 0) {
                ModelVolume* vol = upper->add_volume(upper_mesh);
                vol->name = volume->name;
                vol->config         = volume->config;
                vol->set_material(volume->material_id(), *volume->material());
            }
            if (keep_lower && lower_mesh.facets_count() > 0) {
                ModelVolume* vol = lower->add_volume(lower_mesh);
                vol->name = volume->name;
                vol->config         = volume->config;
                vol->set_material(volume->material_id(), *volume->material());

                // Compute the lower part instances' bounding boxes to figure out where to place
                // the upper part
                if (keep_upper) {
                    for (size_t i = 0; i < instances.size(); i++) {
                        lower_bboxes[i].merge(instances[i]->transform_mesh_bounding_box(lower_mesh, true));
                    }
                }
            }
        }
    }

    ModelObjectPtrs res;

    if (keep_upper && upper->volumes.size() > 0) {
        upper->invalidate_bounding_box();
        upper->center_around_origin();

        // Reset instance transformation except offset and Z-rotation
        for (size_t i = 0; i < instances.size(); i++) {
            auto &instance = upper->instances[i];
            const Vec3d offset = instance->get_offset();
            const double rot_z = instance->get_rotation()(2);
            // The upper part displacement is set to half of the lower part bounding box
            // this is done in hope at least a part of the upper part will always be visible and draggable
            const Vec3d displace = lower_bboxes[i].size().cwiseProduct(Vec3d(-0.5, -0.5, 0.0));

            instance->set_transformation(Geometry::Transformation());
            instance->set_offset(offset + displace);
            instance->set_rotation(Vec3d(0.0, 0.0, rot_z));
        }

        res.push_back(upper);
    }
    if (keep_lower && lower->volumes.size() > 0) {
        lower->invalidate_bounding_box();
        lower->center_around_origin();

        // Reset instance transformation except offset and Z-rotation
        for (auto *instance : lower->instances) {
            const Vec3d offset = instance->get_offset();
            const double rot_z = instance->get_rotation()(2);

            instance->set_transformation(Geometry::Transformation());
            instance->set_offset(offset);
            instance->set_rotation(Vec3d(rotate_lower ? Geometry::deg2rad(180.0) : 0.0, 0.0, rot_z));
        }

        res.push_back(lower);
    }

    BOOST_LOG_TRIVIAL(trace) << "ModelObject::cut - end";

    return res;
}

void ModelObject::split(ModelObjectPtrs* new_objects)
{
    if (this->volumes.size() > 1) {
        // We can't split meshes if there's more than one volume, because
        // we can't group the resulting meshes by object afterwards
        new_objects->emplace_back(this);
        return;
    }
    
    ModelVolume* volume = this->volumes.front();
    TriangleMeshPtrs meshptrs = volume->mesh.split();
    for (TriangleMesh *mesh : meshptrs) {
        mesh->repair();
        
        // XXX: this seems to be the only real usage of m_model, maybe refactor this so that it's not needed?
        ModelObject* new_object = m_model->add_object();    
        new_object->name   = this->name;
        new_object->config = this->config;
        new_object->instances.reserve(this->instances.size());
        for (const ModelInstance *model_instance : this->instances)
            new_object->add_instance(*model_instance);
        ModelVolume* new_vol = new_object->add_volume(*volume, std::move(*mesh));
#if !ENABLE_VOLUMES_CENTERING_FIXES
        new_vol->center_geometry();
#endif // !ENABLE_VOLUMES_CENTERING_FIXES

        for (ModelInstance* model_instance : new_object->instances)
        {
            Vec3d shift = model_instance->get_transformation().get_matrix(true) * new_vol->get_offset();
            model_instance->set_offset(model_instance->get_offset() + shift);
        }

        new_vol->set_offset(Vec3d::Zero());
        new_objects->emplace_back(new_object);
        delete mesh;
    }
    
    return;
}

void ModelObject::repair()
{
    for (ModelVolume *v : this->volumes)
        v->mesh.repair();
}

// Support for non-uniform scaling of instances. If an instance is rotated by angles, which are not multiples of ninety degrees,
// then the scaling in world coordinate system is not representable by the Geometry::Transformation structure.
// This situation is solved by baking in the instance transformation into the mesh vertices.
// Rotation and mirroring is being baked in. In case the instance scaling was non-uniform, it is baked in as well.
void ModelObject::bake_xy_rotation_into_meshes(size_t instance_idx)
{
    assert(instance_idx < this->instances.size());

	const Geometry::Transformation reference_trafo = this->instances[instance_idx]->get_transformation();
    if (Geometry::is_rotation_ninety_degrees(reference_trafo.get_rotation()))
        // nothing to do, scaling in the world coordinate space is possible in the representation of Geometry::Transformation.
        return;

    bool   left_handed        = reference_trafo.is_left_handed();
    bool   has_mirrorring     = ! reference_trafo.get_mirror().isApprox(Vec3d(1., 1., 1.));
    bool   uniform_scaling    = std::abs(reference_trafo.get_scaling_factor().x() - reference_trafo.get_scaling_factor().y()) < EPSILON &&
                                std::abs(reference_trafo.get_scaling_factor().x() - reference_trafo.get_scaling_factor().z()) < EPSILON;
    double new_scaling_factor = uniform_scaling ? reference_trafo.get_scaling_factor().x() : 1.;

    // Adjust the instances.
    for (size_t i = 0; i < this->instances.size(); ++ i) {
        ModelInstance &model_instance = *this->instances[i];
        model_instance.set_rotation(Vec3d(0., 0., Geometry::rotation_diff_z(reference_trafo.get_rotation(), model_instance.get_rotation())));
        model_instance.set_scaling_factor(Vec3d(new_scaling_factor, new_scaling_factor, new_scaling_factor));
        model_instance.set_mirror(Vec3d(1., 1., 1.));
    }

    // Adjust the meshes.
    // Transformation to be applied to the meshes.
    Eigen::Matrix3d    mesh_trafo_3x3           = reference_trafo.get_matrix(true, false, uniform_scaling, ! has_mirrorring).matrix().block<3, 3>(0, 0);
	Transform3d volume_offset_correction = this->instances[instance_idx]->get_transformation().get_matrix().inverse() * reference_trafo.get_matrix();
    for (ModelVolume *model_volume : this->volumes) {
        const Geometry::Transformation volume_trafo = model_volume->get_transformation();
        bool   volume_left_handed        = volume_trafo.is_left_handed();
        bool   volume_has_mirrorring     = ! volume_trafo.get_mirror().isApprox(Vec3d(1., 1., 1.));
        bool   volume_uniform_scaling    = std::abs(volume_trafo.get_scaling_factor().x() - volume_trafo.get_scaling_factor().y()) < EPSILON &&
                                           std::abs(volume_trafo.get_scaling_factor().x() - volume_trafo.get_scaling_factor().z()) < EPSILON;
        double volume_new_scaling_factor = volume_uniform_scaling ? volume_trafo.get_scaling_factor().x() : 1.;
        // Transform the mesh.
		Matrix3d volume_trafo_3x3 = volume_trafo.get_matrix(true, false, volume_uniform_scaling, !volume_has_mirrorring).matrix().block<3, 3>(0, 0);
		model_volume->transform_mesh(mesh_trafo_3x3 * volume_trafo_3x3, left_handed != volume_left_handed);
        // Reset the rotation, scaling and mirroring.
        model_volume->set_rotation(Vec3d(0., 0., 0.));
        model_volume->set_scaling_factor(Vec3d(volume_new_scaling_factor, volume_new_scaling_factor, volume_new_scaling_factor));
        model_volume->set_mirror(Vec3d(1., 1., 1.));
        // Move the reference point of the volume to compensate for the change of the instance trafo.
        model_volume->set_offset(volume_offset_correction * volume_trafo.get_offset());
    }

    this->invalidate_bounding_box();
}

double ModelObject::get_min_z() const
{
    if (instances.empty())
        return 0.0;
    else
    {
        double min_z = DBL_MAX;
        for (size_t i = 0; i < instances.size(); ++i)
        {
            min_z = std::min(min_z, get_instance_min_z(i));
        }
        return min_z;
    }
}

double ModelObject::get_instance_min_z(size_t instance_idx) const
{
    double min_z = DBL_MAX;

    ModelInstance* inst = instances[instance_idx];
    const Transform3d& mi = inst->get_matrix(true);

    for (const ModelVolume* v : volumes)
    {
        if (!v->is_model_part())
            continue;

        Transform3d mv = mi * v->get_matrix();
        const TriangleMesh& hull = v->get_convex_hull();
        for (uint32_t f = 0; f < hull.stl.stats.number_of_facets; ++f)
        {
            const stl_facet* facet = hull.stl.facet_start + f;
            min_z = std::min(min_z, Vec3d::UnitZ().dot(mv * facet->vertex[0].cast<double>()));
            min_z = std::min(min_z, Vec3d::UnitZ().dot(mv * facet->vertex[1].cast<double>()));
            min_z = std::min(min_z, Vec3d::UnitZ().dot(mv * facet->vertex[2].cast<double>()));
        }
    }

    return min_z + inst->get_offset(Z);
}

unsigned int ModelObject::check_instances_print_volume_state(const BoundingBoxf3& print_volume)
{
    unsigned int num_printable = 0;
    enum {
        INSIDE  = 1,
        OUTSIDE = 2
    };
    for (ModelInstance *model_instance : this->instances) {
        unsigned int inside_outside = 0;
        for (const ModelVolume *vol : this->volumes)
            if (vol->is_model_part()) {
                BoundingBoxf3 bb = vol->get_convex_hull().transformed_bounding_box(model_instance->get_matrix() * vol->get_matrix());
                if (print_volume.contains(bb))
                    inside_outside |= INSIDE;
                else if (print_volume.intersects(bb))
                    inside_outside |= INSIDE | OUTSIDE;
                else
                    inside_outside |= OUTSIDE;
            }
        model_instance->print_volume_state = 
            (inside_outside == (INSIDE | OUTSIDE)) ? ModelInstance::PVS_Partly_Outside :
            (inside_outside == INSIDE) ? ModelInstance::PVS_Inside : ModelInstance::PVS_Fully_Outside;
        if (inside_outside == INSIDE)
            ++ num_printable;
    }
    return num_printable;
}

void ModelObject::print_info() const
{
    using namespace std;
    cout << fixed;
    boost::nowide::cout << "[" << boost::filesystem::path(this->input_file).filename().string() << "]" << endl;
    
    TriangleMesh mesh = this->raw_mesh();
    mesh.check_topology();
    BoundingBoxf3 bb = mesh.bounding_box();
    Vec3d size = bb.size();
    cout << "size_x = " << size(0) << endl;
    cout << "size_y = " << size(1) << endl;
    cout << "size_z = " << size(2) << endl;
    cout << "min_x = " << bb.min(0) << endl;
    cout << "min_y = " << bb.min(1) << endl;
    cout << "min_z = " << bb.min(2) << endl;
    cout << "max_x = " << bb.max(0) << endl;
    cout << "max_y = " << bb.max(1) << endl;
    cout << "max_z = " << bb.max(2) << endl;
    cout << "number_of_facets = " << mesh.stl.stats.number_of_facets  << endl;
    cout << "manifold = "   << (mesh.is_manifold() ? "yes" : "no") << endl;
    
    mesh.repair();  // this calculates number_of_parts
    if (mesh.needed_repair()) {
        mesh.repair();
        if (mesh.stl.stats.degenerate_facets > 0)
            cout << "degenerate_facets = "  << mesh.stl.stats.degenerate_facets << endl;
        if (mesh.stl.stats.edges_fixed > 0)
            cout << "edges_fixed = "        << mesh.stl.stats.edges_fixed       << endl;
        if (mesh.stl.stats.facets_removed > 0)
            cout << "facets_removed = "     << mesh.stl.stats.facets_removed    << endl;
        if (mesh.stl.stats.facets_added > 0)
            cout << "facets_added = "       << mesh.stl.stats.facets_added      << endl;
        if (mesh.stl.stats.facets_reversed > 0)
            cout << "facets_reversed = "    << mesh.stl.stats.facets_reversed   << endl;
        if (mesh.stl.stats.backwards_edges > 0)
            cout << "backwards_edges = "    << mesh.stl.stats.backwards_edges   << endl;
    }
    cout << "number_of_parts =  " << mesh.stl.stats.number_of_parts << endl;
    cout << "volume = "           << mesh.volume()                  << endl;
}

std::string ModelObject::get_export_filename() const
{
    std::string ret = input_file;

    if (!name.empty())
    {
        if (ret.empty())
            // input_file was empty, just use name
            ret = name;
        else
        {
            // Replace file name in input_file with name, but keep the path and file extension.
            ret = (boost::filesystem::path(name).parent_path().empty()) ?
                (boost::filesystem::path(ret).parent_path() / name).make_preferred().string() : name;
        }
    }

    return ret;
}

stl_stats ModelObject::get_object_stl_stats() const
{
    if (this->volumes.size() == 1)
        return this->volumes[0]->mesh.stl.stats;

    stl_stats full_stats;
    memset(&full_stats, 0, sizeof(stl_stats));

    // fill full_stats from all objet's meshes
    for (ModelVolume* volume : this->volumes)
    {
        if (volume->id() == this->volumes[0]->id())
            continue;

        const stl_stats& stats = volume->mesh.stl.stats;

        // initialize full_stats (for repaired errors)
        full_stats.degenerate_facets    += stats.degenerate_facets;
        full_stats.edges_fixed          += stats.edges_fixed;
        full_stats.facets_removed       += stats.facets_removed;
        full_stats.facets_added         += stats.facets_added;
        full_stats.facets_reversed      += stats.facets_reversed;
        full_stats.backwards_edges      += stats.backwards_edges;

        // another used satistics value
        if (volume->is_model_part()) {
            full_stats.volume           += stats.volume;
            full_stats.number_of_parts  += stats.number_of_parts;
        }
    }

    return full_stats;
}

int ModelObject::get_mesh_errors_count(const int vol_idx /*= -1*/) const
{
    if (vol_idx >= 0)
        return this->volumes[vol_idx]->get_mesh_errors_count();

    const stl_stats& stats = get_object_stl_stats();

    return  stats.degenerate_facets + stats.edges_fixed     + stats.facets_removed +
            stats.facets_added      + stats.facets_reversed + stats.backwards_edges;
}

void ModelVolume::set_material_id(t_model_material_id material_id)
{
    m_material_id = material_id;
    // ensure m_material_id references an existing material
    if (! material_id.empty())
        this->object->get_model()->add_material(material_id);
}

ModelMaterial* ModelVolume::material() const
{ 
    return this->object->get_model()->get_material(m_material_id);
}

void ModelVolume::set_material(t_model_material_id material_id, const ModelMaterial &material)
{
    m_material_id = material_id;
    if (! material_id.empty())
        this->object->get_model()->add_material(material_id, material);
}

// Extract the current extruder ID based on this ModelVolume's config and the parent ModelObject's config.
int ModelVolume::extruder_id() const
{
    int extruder_id = -1;
    if (this->is_model_part()) {
        const ConfigOption *opt = this->config.option("extruder");
        if ((opt == nullptr) || (opt->getInt() == 0))
            opt = this->object->config.option("extruder");
        extruder_id = (opt == nullptr) ? 0 : opt->getInt();
    }
    return extruder_id;
}

bool ModelVolume::is_splittable() const
{
    // the call mesh.is_splittable() is expensive, so cache the value to calculate it only once
    if (m_is_splittable == -1)
        m_is_splittable = (int)mesh.is_splittable();

    return m_is_splittable == 1;
}

void ModelVolume::center_geometry()
{
#if ENABLE_VOLUMES_CENTERING_FIXES
    Vec3d shift = mesh.bounding_box().center();
    if (!shift.isApprox(Vec3d::Zero()))
    {
        mesh.translate(-(float)shift(0), -(float)shift(1), -(float)shift(2));
        m_convex_hull.translate(-(float)shift(0), -(float)shift(1), -(float)shift(2));
        translate(shift);
    }
#else
    Vec3d shift = -mesh.bounding_box().center();
    mesh.translate((float)shift(0), (float)shift(1), (float)shift(2));
    m_convex_hull.translate((float)shift(0), (float)shift(1), (float)shift(2));
    translate(-shift);
#endif // ENABLE_VOLUMES_CENTERING_FIXES
}

void ModelVolume::calculate_convex_hull()
{
    m_convex_hull = mesh.convex_hull_3d();
}

int ModelVolume::get_mesh_errors_count() const
{
    const stl_stats& stats = this->mesh.stl.stats;

    return  stats.degenerate_facets + stats.edges_fixed     + stats.facets_removed +
            stats.facets_added      + stats.facets_reversed + stats.backwards_edges;
}

const TriangleMesh& ModelVolume::get_convex_hull() const
{
    return m_convex_hull;
}

ModelVolumeType ModelVolume::type_from_string(const std::string &s)
{
    // Legacy support
    if (s == "1")
		return ModelVolumeType::PARAMETER_MODIFIER;
    // New type (supporting the support enforcers & blockers)
    if (s == "ModelPart")
		return ModelVolumeType::MODEL_PART;
    if (s == "ParameterModifier")
		return ModelVolumeType::PARAMETER_MODIFIER;
    if (s == "SupportEnforcer")
		return ModelVolumeType::SUPPORT_ENFORCER;
    if (s == "SupportBlocker")
		return ModelVolumeType::SUPPORT_BLOCKER;
    assert(s == "0");
    // Default value if invalud type string received.
	return ModelVolumeType::MODEL_PART;
}

std::string ModelVolume::type_to_string(const ModelVolumeType t)
{
    switch (t) {
	case ModelVolumeType::MODEL_PART:         return "ModelPart";
	case ModelVolumeType::PARAMETER_MODIFIER: return "ParameterModifier";
	case ModelVolumeType::SUPPORT_ENFORCER:   return "SupportEnforcer";
	case ModelVolumeType::SUPPORT_BLOCKER:    return "SupportBlocker";
    default:
        assert(false);
        return "ModelPart";
    }
}

// Split this volume, append the result to the object owning this volume.
// Return the number of volumes created from this one.
// This is useful to assign different materials to different volumes of an object.
size_t ModelVolume::split(unsigned int max_extruders)
{
    TriangleMeshPtrs meshptrs = this->mesh.split();
    if (meshptrs.size() <= 1) {
        delete meshptrs.front();
        return 1;
    }

    size_t idx = 0;
    size_t ivolume = std::find(this->object->volumes.begin(), this->object->volumes.end(), this) - this->object->volumes.begin();
    std::string name = this->name;

    Model::reset_auto_extruder_id();
    Vec3d offset = this->get_offset();

    for (TriangleMesh *mesh : meshptrs) {
        mesh->repair();
        if (idx == 0)
        {
            this->mesh = std::move(*mesh);
            this->calculate_convex_hull();
            // Assign a new unique ID, so that a new GLVolume will be generated.
            this->set_new_unique_id();
        }
        else
            this->object->volumes.insert(this->object->volumes.begin() + (++ivolume), new ModelVolume(object, *this, std::move(*mesh)));

        this->object->volumes[ivolume]->set_offset(Vec3d::Zero());
        this->object->volumes[ivolume]->center_geometry();
        this->object->volumes[ivolume]->translate(offset);
        this->object->volumes[ivolume]->name = name + "_" + std::to_string(idx + 1);
        this->object->volumes[ivolume]->config.set_deserialize("extruder", Model::get_auto_extruder_id_as_string(max_extruders));
        delete mesh;
        ++ idx;
    }
    
    return idx;
}

void ModelVolume::translate(const Vec3d& displacement)
{
    set_offset(get_offset() + displacement);
}

void ModelVolume::scale(const Vec3d& scaling_factors)
{
    set_scaling_factor(get_scaling_factor().cwiseProduct(scaling_factors));
}

void ModelObject::scale_to_fit(const Vec3d &size)
{
/*
    BoundingBoxf3 instance_bounding_box(size_t instance_idx, bool dont_translate = false) const;
    Vec3d orig_size = this->bounding_box().size();
    float factor = fminf(
        size.x / orig_size.x,
        fminf(
            size.y / orig_size.y,
            size.z / orig_size.z
        )
    );
    this->scale(factor);
*/
}

void ModelVolume::rotate(double angle, Axis axis)
{
    switch (axis)
    {
    case X: { rotate(angle, Vec3d::UnitX()); break; }
    case Y: { rotate(angle, Vec3d::UnitY()); break; }
    case Z: { rotate(angle, Vec3d::UnitZ()); break; }
    default: break;
    }
}

void ModelVolume::rotate(double angle, const Vec3d& axis)
{
    set_rotation(get_rotation() + Geometry::extract_euler_angles(Eigen::Quaterniond(Eigen::AngleAxisd(angle, axis)).toRotationMatrix()));
}

void ModelVolume::mirror(Axis axis)
{
    Vec3d mirror = get_mirror();
    switch (axis)
    {
    case X: { mirror(0) *= -1.0; break; }
    case Y: { mirror(1) *= -1.0; break; }
    case Z: { mirror(2) *= -1.0; break; }
    default: break;
    }
    set_mirror(mirror);
}

void ModelVolume::scale_geometry(const Vec3d& versor)
{
    mesh.scale(versor);
    m_convex_hull.scale(versor);
}

void ModelVolume::transform_mesh(const Transform3d &mesh_trafo, bool fix_left_handed)
{
    this->mesh.transform(mesh_trafo, fix_left_handed);
    this->m_convex_hull.transform(mesh_trafo, fix_left_handed);
    // Let the rest of the application know that the geometry changed, so the meshes have to be reloaded.
    this->set_new_unique_id();
}

void ModelVolume::transform_mesh(const Matrix3d &matrix, bool fix_left_handed)
{
	this->mesh.transform(matrix, fix_left_handed);
	this->m_convex_hull.transform(matrix, fix_left_handed);
    // Let the rest of the application know that the geometry changed, so the meshes have to be reloaded.
    this->set_new_unique_id();
}

void ModelInstance::transform_mesh(TriangleMesh* mesh, bool dont_translate) const
{
    mesh->transform(get_matrix(dont_translate));
}

BoundingBoxf3 ModelInstance::transform_mesh_bounding_box(const TriangleMesh& mesh, bool dont_translate) const
{
    // Rotate around mesh origin.
    TriangleMesh copy(mesh);
    copy.transform(get_matrix(true, false, true, true));
    BoundingBoxf3 bbox = copy.bounding_box();

    if (!empty(bbox)) {
        // Scale the bounding box along the three axes.
        for (unsigned int i = 0; i < 3; ++i)
        {
            if (std::abs(get_scaling_factor((Axis)i)-1.0) > EPSILON)
            {
                bbox.min(i) *= get_scaling_factor((Axis)i);
                bbox.max(i) *= get_scaling_factor((Axis)i);
            }
        }

        // Translate the bounding box.
        if (! dont_translate) {
            bbox.min += get_offset();
            bbox.max += get_offset();
        }
    }
    return bbox;
}

BoundingBoxf3 ModelInstance::transform_bounding_box(const BoundingBoxf3 &bbox, bool dont_translate) const
{
    return bbox.transformed(get_matrix(dont_translate));
}

Vec3d ModelInstance::transform_vector(const Vec3d& v, bool dont_translate) const
{
    return get_matrix(dont_translate) * v;
}

void ModelInstance::transform_polygon(Polygon* polygon) const
{
    // CHECK_ME -> Is the following correct or it should take in account all three rotations ?
    polygon->rotate(get_rotation(Z)); // rotate around polygon origin
    // CHECK_ME -> Is the following correct ?
    polygon->scale(get_scaling_factor(X), get_scaling_factor(Y)); // scale around polygon origin
}

// Test whether the two models contain the same number of ModelObjects with the same set of IDs
// ordered in the same order. In that case it is not necessary to kill the background processing.
bool model_object_list_equal(const Model &model_old, const Model &model_new)
{
    if (model_old.objects.size() != model_new.objects.size())
        return false;
    for (size_t i = 0; i < model_old.objects.size(); ++ i)
        if (model_old.objects[i]->id() != model_new.objects[i]->id())
            return false;
    return true;
}

// Test whether the new model is just an extension of the old model (new objects were added
// to the end of the original list. In that case it is not necessary to kill the background processing.
bool model_object_list_extended(const Model &model_old, const Model &model_new)
{
    if (model_old.objects.size() >= model_new.objects.size())
        return false;
    for (size_t i = 0; i < model_old.objects.size(); ++ i)
        if (model_old.objects[i]->id() != model_new.objects[i]->id())
            return false;
    return true;
}

bool model_volume_list_changed(const ModelObject &model_object_old, const ModelObject &model_object_new, const ModelVolumeType type)
{
    size_t i_old, i_new;
    for (i_old = 0, i_new = 0; i_old < model_object_old.volumes.size() && i_new < model_object_new.volumes.size();) {
        const ModelVolume &mv_old = *model_object_old.volumes[i_old];
        const ModelVolume &mv_new = *model_object_new.volumes[i_new];
        if (mv_old.type() != type) {
            ++ i_old;
            continue;
        }
        if (mv_new.type() != type) {
            ++ i_new;
            continue;
        }
        if (mv_old.id() != mv_new.id())
            return true;
        //FIXME test for the content of the mesh!

        if (!mv_old.get_matrix().isApprox(mv_new.get_matrix()))
            return true;

        ++i_old;
        ++ i_new;
    }
    for (; i_old < model_object_old.volumes.size(); ++ i_old) {
        const ModelVolume &mv_old = *model_object_old.volumes[i_old];
        if (mv_old.type() == type)
            // ModelVolume was deleted.
            return true;
    }
    for (; i_new < model_object_new.volumes.size(); ++ i_new) {
        const ModelVolume &mv_new = *model_object_new.volumes[i_new];
        if (mv_new.type() == type)
            // ModelVolume was added.
            return true;
    }
    return false;
}

#ifndef NDEBUG
// Verify whether the IDs of Model / ModelObject / ModelVolume / ModelInstance / ModelMaterial are valid and unique.
void check_model_ids_validity(const Model &model)
{
    std::set<ModelID> ids;
    auto check = [&ids](ModelID id) { 
        assert(id.id > 0);
        assert(ids.find(id) == ids.end());
        ids.insert(id);
    };
    for (const ModelObject *model_object : model.objects) {
        check(model_object->id());
        for (const ModelVolume *model_volume : model_object->volumes)
            check(model_volume->id());
        for (const ModelInstance *model_instance : model_object->instances)
            check(model_instance->id());
    }
    for (const auto mm : model.materials)
        check(mm.second->id());
}

void check_model_ids_equal(const Model &model1, const Model &model2)
{
    // Verify whether the IDs of model1 and model match.
    assert(model1.objects.size() == model2.objects.size());
    for (size_t idx_model = 0; idx_model < model2.objects.size(); ++ idx_model) {
        const ModelObject &model_object1 = *model1.objects[idx_model];
        const ModelObject &model_object2 = *  model2.objects[idx_model];
        assert(model_object1.id() == model_object2.id());
        assert(model_object1.volumes.size() == model_object2.volumes.size());
        assert(model_object1.instances.size() == model_object2.instances.size());
        for (size_t i = 0; i < model_object1.volumes.size(); ++ i)
            assert(model_object1.volumes[i]->id() == model_object2.volumes[i]->id());
        for (size_t i = 0; i < model_object1.instances.size(); ++ i)
            assert(model_object1.instances[i]->id() == model_object2.instances[i]->id());
    }
    assert(model1.materials.size() == model2.materials.size());
    {
        auto it1 = model1.materials.begin();
        auto it2 = model2.materials.begin();
        for (; it1 != model1.materials.end(); ++ it1, ++ it2) {
            assert(it1->first == it2->first); // compare keys
            assert(it1->second->id() == it2->second->id());
        }
    }
}
#endif /* NDEBUG */

}

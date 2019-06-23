#ifndef slic3r_Model_hpp_
#define slic3r_Model_hpp_

#include "libslic3r.h"
#include "PrintConfig.hpp"
#include "Layer.hpp"
#include "Point.hpp"
#include "TriangleMesh.hpp"
#include "Slicing.hpp"
#include <map>
#include <string>
#include <utility>
#include <vector>
#include "Geometry.hpp"
#include <libslic3r/SLA/SLACommon.hpp>

namespace Slic3r {

class Model;
class ModelInstance;
class ModelMaterial;
class ModelObject;
class ModelVolume;
class Print;
class SLAPrint;

typedef std::string t_model_material_id;
typedef std::string t_model_material_attribute;
typedef std::map<t_model_material_attribute, std::string> t_model_material_attributes;

typedef std::map<t_model_material_id, ModelMaterial*> ModelMaterialMap;
typedef std::vector<ModelObject*> ModelObjectPtrs;
typedef std::vector<ModelVolume*> ModelVolumePtrs;
typedef std::vector<ModelInstance*> ModelInstancePtrs;

// Unique identifier of a Model, ModelObject, ModelVolume, ModelInstance or ModelMaterial.
// Used to synchronize the front end (UI) with the back end (BackgroundSlicingProcess / Print / PrintObject)
// Valid IDs are strictly positive (non zero).
// It is declared as an object, as some compilers (notably msvcc) consider a typedef size_t equivalent to size_t
// for parameter overload.
struct ModelID 
{
	ModelID(size_t id) : id(id) {}

	bool operator==(const ModelID &rhs) const { return this->id == rhs.id; }
	bool operator!=(const ModelID &rhs) const { return this->id != rhs.id; }
	bool operator< (const ModelID &rhs) const { return this->id <  rhs.id; }
	bool operator> (const ModelID &rhs) const { return this->id >  rhs.id; }
	bool operator<=(const ModelID &rhs) const { return this->id <= rhs.id; }
	bool operator>=(const ModelID &rhs) const { return this->id >= rhs.id; }

    bool valid() const { return id != 0; }

	size_t	id;
};

// Unique object / instance ID for the wipe tower.
extern ModelID wipe_tower_object_id();
extern ModelID wipe_tower_instance_id();

// Base for Model, ModelObject, ModelVolume, ModelInstance or ModelMaterial to provide a unique ID
// to synchronize the front end (UI) with the back end (BackgroundSlicingProcess / Print / PrintObject).
// Achtung! The s_last_id counter is not thread safe, so it is expected, that the ModelBase derived instances
// are only instantiated from the main thread.
class ModelBase
{
public:
    ModelID     id() const { return m_id; }

protected:
    // Constructors to be only called by derived classes.
    // Default constructor to assign a unique ID.
    ModelBase() : m_id(generate_new_id()) {}
    // Constructor with ignored int parameter to assign an invalid ID, to be replaced
    // by an existing ID copied from elsewhere.
    ModelBase(int) : m_id(ModelID(0)) {}

    // Use with caution!
    void        set_new_unique_id() { m_id = generate_new_id(); }
    void        set_invalid_id()    { m_id = 0; }
    // Use with caution!
    void        copy_id(const ModelBase &rhs) { m_id = rhs.id(); }

    // Override this method if a ModelBase derived class owns other ModelBase derived instances.
    void        assign_new_unique_ids_recursive() { this->set_new_unique_id(); }

private:
    ModelID                 m_id;

	static inline ModelID   generate_new_id() { return ModelID(++ s_last_id); }
    static size_t           s_last_id;
	
	friend ModelID wipe_tower_object_id();
	friend ModelID wipe_tower_instance_id();
};

#define MODELBASE_DERIVED_COPY_MOVE_CLONE(TYPE) \
    /* Copy a model, copy the IDs. The Print::apply() will call the TYPE::copy() method */ \
    /* to make a private copy for background processing. */ \
    static TYPE* new_copy(const TYPE &rhs)  { return new TYPE(rhs); } \
    static TYPE* new_copy(TYPE &&rhs)       { return new TYPE(std::move(rhs)); } \
    static TYPE  make_copy(const TYPE &rhs) { return TYPE(rhs); } \
    static TYPE  make_copy(TYPE &&rhs)      { return TYPE(std::move(rhs)); } \
    TYPE&        assign_copy(const TYPE &rhs); \
    TYPE&        assign_copy(TYPE &&rhs); \
    /* Copy a TYPE, generate new IDs. The front end will use this call. */ \
    static TYPE* new_clone(const TYPE &rhs) { \
        /* Default constructor assigning an invalid ID. */ \
        auto obj = new TYPE(-1); \
        obj->assign_clone(rhs); \
        return obj; \
	} \
    TYPE         make_clone(const TYPE &rhs) { \
        /* Default constructor assigning an invalid ID. */ \
        TYPE obj(-1); \
        obj.assign_clone(rhs); \
        return obj; \
    } \
    TYPE&        assign_clone(const TYPE &rhs) { \
        this->assign_copy(rhs); \
        this->assign_new_unique_ids_recursive(); \
		return *this; \
    }

#define MODELBASE_DERIVED_PRIVATE_COPY_MOVE(TYPE) \
private: \
    /* Private constructor with an unused int parameter will create a TYPE instance with an invalid ID. */ \
    explicit TYPE(int) : ModelBase(-1) {}; \
    void assign_new_unique_ids_recursive();

// Material, which may be shared across multiple ModelObjects of a single Model.
class ModelMaterial : public ModelBase
{
public:
    // Attributes are defined by the AMF file format, but they don't seem to be used by Slic3r for any purpose.
    t_model_material_attributes attributes;
    // Dynamic configuration storage for the object specific configuration values, overriding the global configuration.
    DynamicPrintConfig config;

    Model* get_model() const { return m_model; }
    void apply(const t_model_material_attributes &attributes)
        { this->attributes.insert(attributes.begin(), attributes.end()); }

protected:
    friend class Model;
	// Constructor, which assigns a new unique ID.
	ModelMaterial(Model *model) : m_model(model) {}
	// Copy constructor copies the ID and m_model!
	ModelMaterial(const ModelMaterial &rhs) = default;
	void set_model(Model *model) { m_model = model; }

private:
    // Parent, owning this material.
    Model *m_model;
    
	ModelMaterial() = delete;
	ModelMaterial(ModelMaterial &&rhs) = delete;
	ModelMaterial& operator=(const ModelMaterial &rhs) = delete;
    ModelMaterial& operator=(ModelMaterial &&rhs) = delete;
};

// A printable object, possibly having multiple print volumes (each with its own set of parameters and materials),
// and possibly having multiple modifier volumes, each modifier volume with its set of parameters and materials.
// Each ModelObject may be instantiated mutliple times, each instance having different placement on the print bed,
// different rotation and different uniform scaling.
class ModelObject : public ModelBase
{
    friend class Model;
public:
    std::string             name;
    std::string             input_file;    // XXX: consider fs::path
    // Instances of this ModelObject. Each instance defines a shift on the print bed, rotation around the Z axis and a uniform scaling.
    // Instances are owned by this ModelObject.
    ModelInstancePtrs       instances;
    // Printable and modifier volumes, each with its material ID and a set of override parameters.
    // ModelVolumes are owned by this ModelObject.
    ModelVolumePtrs         volumes;
    // Configuration parameters specific to a single ModelObject, overriding the global Slic3r settings.
    DynamicPrintConfig      config;
    // Variation of a layer thickness for spans of Z coordinates.
    t_layer_height_ranges   layer_height_ranges;
    // Profile of increasing z to a layer height, to be linearly interpolated when calculating the layers.
    // The pairs of <z, layer_height> are packed into a 1D array.
    std::vector<coordf_t>   layer_height_profile;

    // This vector holds position of selected support points for SLA. The data are
    // saved in mesh coordinates to allow using them for several instances.
    // The format is (x, y, z, point_size, supports_island)
    std::vector<sla::SupportPoint>      sla_support_points;
    // To keep track of where the points came from (used for synchronization between
    // the SLA gizmo and the backend).
    sla::PointsStatus sla_points_status = sla::PointsStatus::None;

    /* This vector accumulates the total translation applied to the object by the
        center_around_origin() method. Callers might want to apply the same translation
        to new volumes before adding them to this object in order to preserve alignment
        when user expects that. */
    Vec3d                   origin_translation;

    Model*                  get_model() { return m_model; };
	const Model*            get_model() const { return m_model; };

    ModelVolume*            add_volume(const TriangleMesh &mesh);
    ModelVolume*            add_volume(TriangleMesh &&mesh);
    ModelVolume*            add_volume(const ModelVolume &volume);
    ModelVolume*            add_volume(const ModelVolume &volume, TriangleMesh &&mesh);
    void                    delete_volume(size_t idx);
    void                    clear_volumes();
    bool                    is_multiparts() const { return volumes.size() > 1; }

    ModelInstance*          add_instance();
    ModelInstance*          add_instance(const ModelInstance &instance);
    ModelInstance*          add_instance(const Vec3d &offset, const Vec3d &scaling_factor, const Vec3d &rotation, const Vec3d &mirror);
    void                    delete_instance(size_t idx);
    void                    delete_last_instance();
    void                    clear_instances();

    // Returns the bounding box of the transformed instances.
    // This bounding box is approximate and not snug.
    // This bounding box is being cached.
    const BoundingBoxf3& bounding_box() const;
    void invalidate_bounding_box() { m_bounding_box_valid = false; m_raw_bounding_box_valid = false; m_raw_mesh_bounding_box_valid = false; }

    // A mesh containing all transformed instances of this object.
    TriangleMesh mesh() const;
    // Non-transformed (non-rotated, non-scaled, non-translated) sum of non-modifier object volumes.
    // Currently used by ModelObject::mesh() and to calculate the 2D envelope for 2D platter.
    TriangleMesh raw_mesh() const;
    // Non-transformed (non-rotated, non-scaled, non-translated) sum of all object volumes.
    TriangleMesh full_raw_mesh() const;
    // A transformed snug bounding box around the non-modifier object volumes, without the translation applied.
    // This bounding box is only used for the actual slicing.
    const BoundingBoxf3& raw_bounding_box() const;
    // A snug bounding box around the transformed non-modifier object volumes.
    BoundingBoxf3 instance_bounding_box(size_t instance_idx, bool dont_translate = false) const;
	// A snug bounding box of non-transformed (non-rotated, non-scaled, non-translated) sum of non-modifier object volumes.
	const BoundingBoxf3& raw_mesh_bounding_box() const;
	// A snug bounding box of non-transformed (non-rotated, non-scaled, non-translated) sum of all object volumes.
    BoundingBoxf3 full_raw_mesh_bounding_box() const;

    // Calculate 2D convex hull of of a projection of the transformed printable volumes into the XY plane.
    // This method is cheap in that it does not make any unnecessary copy of the volume meshes.
    // This method is used by the auto arrange function.
    Polygon       convex_hull_2d(const Transform3d &trafo_instance) const;

#if ENABLE_VOLUMES_CENTERING_FIXES
    void center_around_origin(bool include_modifiers = true);
#else
    void center_around_origin();
#endif // ENABLE_VOLUMES_CENTERING_FIXES
    void ensure_on_bed();
    void translate_instances(const Vec3d& vector);
    void translate_instance(size_t instance_idx, const Vec3d& vector);
    void translate(const Vec3d &vector) { this->translate(vector(0), vector(1), vector(2)); }
    void translate(double x, double y, double z);
    void scale(const Vec3d &versor);
    void scale(const double s) { this->scale(Vec3d(s, s, s)); }
    void scale(double x, double y, double z) { this->scale(Vec3d(x, y, z)); }
    /// Scale the current ModelObject to fit by altering the scaling factor of ModelInstances.
    /// It operates on the total size by duplicating the object according to all the instances.
    /// \param size Sizef3 the size vector
    void scale_to_fit(const Vec3d &size);
    void rotate(double angle, Axis axis);
    void rotate(double angle, const Vec3d& axis);
    void mirror(Axis axis);

    void scale_mesh(const Vec3d& versor);

    size_t materials_count() const;
    size_t facets_count() const;
    bool needed_repair() const;
    ModelObjectPtrs cut(size_t instance, coordf_t z, bool keep_upper = true, bool keep_lower = true, bool rotate_lower = false);    // Note: z is in world coordinates
    void split(ModelObjectPtrs* new_objects);
    void repair();
    // Support for non-uniform scaling of instances. If an instance is rotated by angles, which are not multiples of ninety degrees,
    // then the scaling in world coordinate system is not representable by the Geometry::Transformation structure.
    // This situation is solved by baking in the instance transformation into the mesh vertices.
    // Rotation and mirroring is being baked in. In case the instance scaling was non-uniform, it is baked in as well.
    void bake_xy_rotation_into_meshes(size_t instance_idx);

    double get_min_z() const;
    double get_instance_min_z(size_t instance_idx) const;

    // Called by Print::validate() from the UI thread.
    unsigned int check_instances_print_volume_state(const BoundingBoxf3& print_volume);

    // Print object statistics to console.
    void print_info() const;

    std::string get_export_filename() const;

    // Get full stl statistics for all object's meshes 
    stl_stats   get_object_stl_stats() const;
    // Get count of errors in the mesh( or all object's meshes, if volume index isn't defined) 
    int         get_mesh_errors_count(const int vol_idx = -1) const;

protected:
    friend class Print;
    friend class SLAPrint;
    // Called by Print::apply() to set the model pointer after making a copy.
    void        set_model(Model *model) { m_model = model; }

private:
    ModelObject(Model *model) : m_model(model), origin_translation(Vec3d::Zero()), 
        m_bounding_box_valid(false), m_raw_bounding_box_valid(false), m_raw_mesh_bounding_box_valid(false) {}
    ~ModelObject();

    /* To be able to return an object from own copy / clone methods. Hopefully the compiler will do the "Copy elision" */
    /* (Omits copy and move(since C++11) constructors, resulting in zero - copy pass - by - value semantics). */
    ModelObject(const ModelObject &rhs) : ModelBase(-1), m_model(rhs.m_model) { this->assign_copy(rhs); }
    explicit ModelObject(ModelObject &&rhs) : ModelBase(-1) { this->assign_copy(std::move(rhs)); }
    ModelObject& operator=(const ModelObject &rhs) { this->assign_copy(rhs); m_model = rhs.m_model; return *this; }
    ModelObject& operator=(ModelObject &&rhs) { this->assign_copy(std::move(rhs)); m_model = rhs.m_model; return *this; }

    MODELBASE_DERIVED_COPY_MOVE_CLONE(ModelObject)
	MODELBASE_DERIVED_PRIVATE_COPY_MOVE(ModelObject)

    // Parent object, owning this ModelObject. Set to nullptr here, so the macros above will have it initialized.
    Model                *m_model = nullptr;

    // Bounding box, cached.
    mutable BoundingBoxf3 m_bounding_box;
    mutable bool          m_bounding_box_valid;
    mutable BoundingBoxf3 m_raw_bounding_box;
    mutable bool          m_raw_bounding_box_valid;
    mutable BoundingBoxf3 m_raw_mesh_bounding_box;
    mutable bool          m_raw_mesh_bounding_box_valid;    
};

// Declared outside of ModelVolume, so it could be forward declared.
enum class ModelVolumeType : int {
    INVALID = -1,
    MODEL_PART = 0,
    PARAMETER_MODIFIER,
    SUPPORT_ENFORCER,
    SUPPORT_BLOCKER,
};

// An object STL, or a modifier volume, over which a different set of parameters shall be applied.
// ModelVolume instances are owned by a ModelObject.
class ModelVolume : public ModelBase
{
public:
    std::string         name;
    // The triangular model.
    TriangleMesh        mesh;
    // Configuration parameters specific to an object model geometry or a modifier volume, 
    // overriding the global Slic3r settings and the ModelObject settings.
    DynamicPrintConfig  config;

    // A parent object owning this modifier volume.
    ModelObject*        get_object() const { return this->object; };
    ModelVolumeType     type() const { return m_type; }
    void                set_type(const ModelVolumeType t) { m_type = t; }
	bool                is_model_part()         const { return m_type == ModelVolumeType::MODEL_PART; }
	bool                is_modifier()           const { return m_type == ModelVolumeType::PARAMETER_MODIFIER; }
	bool                is_support_enforcer()   const { return m_type == ModelVolumeType::SUPPORT_ENFORCER; }
	bool                is_support_blocker()    const { return m_type == ModelVolumeType::SUPPORT_BLOCKER; }
	bool                is_support_modifier()   const { return m_type == ModelVolumeType::SUPPORT_BLOCKER || m_type == ModelVolumeType::SUPPORT_ENFORCER; }
    t_model_material_id material_id() const { return m_material_id; }
    void                set_material_id(t_model_material_id material_id);
    ModelMaterial*      material() const;
    void                set_material(t_model_material_id material_id, const ModelMaterial &material);
    // Extract the current extruder ID based on this ModelVolume's config and the parent ModelObject's config.
    // Extruder ID is only valid for FFF. Returns -1 for SLA or if the extruder ID is not applicable (support volumes).
    int                 extruder_id() const;

    bool                is_splittable() const;

    // Split this volume, append the result to the object owning this volume.
    // Return the number of volumes created from this one.
    // This is useful to assign different materials to different volumes of an object.
    size_t              split(unsigned int max_extruders);
    void                translate(double x, double y, double z) { translate(Vec3d(x, y, z)); }
    void                translate(const Vec3d& displacement);
    void                scale(const Vec3d& scaling_factors);
    void                scale(double x, double y, double z) { scale(Vec3d(x, y, z)); }
    void                scale(double s) { scale(Vec3d(s, s, s)); }
    void                rotate(double angle, Axis axis);
    void                rotate(double angle, const Vec3d& axis);
    void                mirror(Axis axis);

    void                scale_geometry(const Vec3d& versor);

    // translates the mesh and the convex hull so that the origin of their vertices is in the center of this volume's bounding box
    void                center_geometry();

    void                calculate_convex_hull();
    const TriangleMesh& get_convex_hull() const;
    // Get count of errors in the mesh
    int                 get_mesh_errors_count() const;

    // Helpers for loading / storing into AMF / 3MF files.
    static ModelVolumeType type_from_string(const std::string &s);
    static std::string  type_to_string(const ModelVolumeType t);

    const Geometry::Transformation& get_transformation() const { return m_transformation; }
    void set_transformation(const Geometry::Transformation& transformation) { m_transformation = transformation; }

    const Vec3d& get_offset() const { return m_transformation.get_offset(); }
    double get_offset(Axis axis) const { return m_transformation.get_offset(axis); }

    void set_offset(const Vec3d& offset) { m_transformation.set_offset(offset); }
    void set_offset(Axis axis, double offset) { m_transformation.set_offset(axis, offset); }

    const Vec3d& get_rotation() const { return m_transformation.get_rotation(); }
    double get_rotation(Axis axis) const { return m_transformation.get_rotation(axis); }

    void set_rotation(const Vec3d& rotation) { m_transformation.set_rotation(rotation); }
    void set_rotation(Axis axis, double rotation) { m_transformation.set_rotation(axis, rotation); }

    Vec3d get_scaling_factor() const { return m_transformation.get_scaling_factor(); }
    double get_scaling_factor(Axis axis) const { return m_transformation.get_scaling_factor(axis); }

    void set_scaling_factor(const Vec3d& scaling_factor) { m_transformation.set_scaling_factor(scaling_factor); }
    void set_scaling_factor(Axis axis, double scaling_factor) { m_transformation.set_scaling_factor(axis, scaling_factor); }

    const Vec3d& get_mirror() const { return m_transformation.get_mirror(); }
    double get_mirror(Axis axis) const { return m_transformation.get_mirror(axis); }
    bool is_left_handed() const { return m_transformation.is_left_handed(); }

    void set_mirror(const Vec3d& mirror) { m_transformation.set_mirror(mirror); }
    void set_mirror(Axis axis, double mirror) { m_transformation.set_mirror(axis, mirror); }

    const Transform3d& get_matrix(bool dont_translate = false, bool dont_rotate = false, bool dont_scale = false, bool dont_mirror = false) const { return m_transformation.get_matrix(dont_translate, dont_rotate, dont_scale, dont_mirror); }

    using ModelBase::set_new_unique_id;

protected:
	friend class Print;
    friend class SLAPrint;
	friend class ModelObject;

	explicit ModelVolume(const ModelVolume &rhs) = default;
    void     set_model_object(ModelObject *model_object) { object = model_object; }
    void     transform_mesh(const Transform3d& t, bool fix_left_handed);
    void     transform_mesh(const Matrix3d& m, bool fix_left_handed);

private:
    // Parent object owning this ModelVolume.
    ModelObject*            object;
    // Is it an object to be printed, or a modifier volume?
    ModelVolumeType         m_type;
    t_model_material_id     m_material_id;
    // The convex hull of this model's mesh.
    TriangleMesh             m_convex_hull;
    Geometry::Transformation m_transformation;

    // flag to optimize the checking if the volume is splittable
    //     -1   ->   is unknown value (before first cheking)
    //      0   ->   is not splittable
    //      1   ->   is splittable
    mutable int               m_is_splittable{ -1 };

	ModelVolume(ModelObject *object, const TriangleMesh &mesh) : mesh(mesh), m_type(ModelVolumeType::MODEL_PART), object(object)
    {
        if (mesh.stl.stats.number_of_facets > 1)
            calculate_convex_hull();
    }
    ModelVolume(ModelObject *object, TriangleMesh &&mesh, TriangleMesh &&convex_hull) :
		mesh(std::move(mesh)), m_convex_hull(std::move(convex_hull)), m_type(ModelVolumeType::MODEL_PART), object(object) {}

    // Copying an existing volume, therefore this volume will get a copy of the ID assigned.
    ModelVolume(ModelObject *object, const ModelVolume &other) :
        ModelBase(other), // copy the ID
        name(other.name), mesh(other.mesh), m_convex_hull(other.m_convex_hull), config(other.config), m_type(other.m_type), object(object), m_transformation(other.m_transformation)
    {
        this->set_material_id(other.material_id());
    }
    // Providing a new mesh, therefore this volume will get a new unique ID assigned.
    ModelVolume(ModelObject *object, const ModelVolume &other, const TriangleMesh &&mesh) :
        name(other.name), mesh(std::move(mesh)), config(other.config), m_type(other.m_type), object(object), m_transformation(other.m_transformation)
    {
        this->set_material_id(other.material_id());
        if (mesh.stl.stats.number_of_facets > 1)
            calculate_convex_hull();
    }

    ModelVolume& operator=(ModelVolume &rhs) = delete;
};

// A single instance of a ModelObject.
// Knows the affine transformation of an object.
class ModelInstance : public ModelBase
{
public:
    enum EPrintVolumeState : unsigned char
    {
        PVS_Inside,
        PVS_Partly_Outside,
        PVS_Fully_Outside,
        Num_BedStates
    };

private:
    Geometry::Transformation m_transformation;

public:
    // flag showing the position of this instance with respect to the print volume (set by Print::validate() using ModelObject::check_instances_print_volume_state())
    EPrintVolumeState print_volume_state;

    ModelObject* get_object() const { return this->object; }

    const Geometry::Transformation& get_transformation() const { return m_transformation; }
    void set_transformation(const Geometry::Transformation& transformation) { m_transformation = transformation; }

    const Vec3d& get_offset() const { return m_transformation.get_offset(); }
    double get_offset(Axis axis) const { return m_transformation.get_offset(axis); }

    void set_offset(const Vec3d& offset) { m_transformation.set_offset(offset); }
    void set_offset(Axis axis, double offset) { m_transformation.set_offset(axis, offset); }

    const Vec3d& get_rotation() const { return m_transformation.get_rotation(); }
    double get_rotation(Axis axis) const { return m_transformation.get_rotation(axis); }

    void set_rotation(const Vec3d& rotation) { m_transformation.set_rotation(rotation); }
    void set_rotation(Axis axis, double rotation) { m_transformation.set_rotation(axis, rotation); }

    const Vec3d& get_scaling_factor() const { return m_transformation.get_scaling_factor(); }
    double get_scaling_factor(Axis axis) const { return m_transformation.get_scaling_factor(axis); }

    void set_scaling_factor(const Vec3d& scaling_factor) { m_transformation.set_scaling_factor(scaling_factor); }
    void set_scaling_factor(Axis axis, double scaling_factor) { m_transformation.set_scaling_factor(axis, scaling_factor); }

    const Vec3d& get_mirror() const { return m_transformation.get_mirror(); }
    double get_mirror(Axis axis) const { return m_transformation.get_mirror(axis); }
	bool is_left_handed() const { return m_transformation.is_left_handed(); }

    void set_mirror(const Vec3d& mirror) { m_transformation.set_mirror(mirror); }
    void set_mirror(Axis axis, double mirror) { m_transformation.set_mirror(axis, mirror); }

    // To be called on an external mesh
    void transform_mesh(TriangleMesh* mesh, bool dont_translate = false) const;
    // Calculate a bounding box of a transformed mesh. To be called on an external mesh.
    BoundingBoxf3 transform_mesh_bounding_box(const TriangleMesh& mesh, bool dont_translate = false) const;
    // Transform an external bounding box.
    BoundingBoxf3 transform_bounding_box(const BoundingBoxf3 &bbox, bool dont_translate = false) const;
    // Transform an external vector.
    Vec3d transform_vector(const Vec3d& v, bool dont_translate = false) const;
    // To be called on an external polygon. It does not translate the polygon, only rotates and scales.
    void transform_polygon(Polygon* polygon) const;

    const Transform3d& get_matrix(bool dont_translate = false, bool dont_rotate = false, bool dont_scale = false, bool dont_mirror = false) const { return m_transformation.get_matrix(dont_translate, dont_rotate, dont_scale, dont_mirror); }

    bool is_printable() const { return print_volume_state == PVS_Inside; }

protected:
    friend class Print;
    friend class SLAPrint;
    friend class ModelObject;

    explicit ModelInstance(const ModelInstance &rhs) = default;
    void     set_model_object(ModelObject *model_object) { object = model_object; }

private:
    // Parent object, owning this instance.
    ModelObject* object;

    // Constructor, which assigns a new unique ID.
    explicit ModelInstance(ModelObject *object) : object(object), print_volume_state(PVS_Inside) {}
    // Constructor, which assigns a new unique ID.
    explicit ModelInstance(ModelObject *object, const ModelInstance &other) :
        m_transformation(other.m_transformation), object(object), print_volume_state(PVS_Inside) {}

    ModelInstance() = delete;
    explicit ModelInstance(ModelInstance &&rhs) = delete;
    ModelInstance& operator=(const ModelInstance &rhs) = delete;
    ModelInstance& operator=(ModelInstance &&rhs) = delete;
};

// The print bed content.
// Description of a triangular model with multiple materials, multiple instances with various affine transformations
// and with multiple modifier meshes.
// A model groups multiple objects, each object having possibly multiple instances,
// all objects may share mutliple materials.
class Model : public ModelBase
{
    static unsigned int s_auto_extruder_id;

public:
    // Materials are owned by a model and referenced by objects through t_model_material_id.
    // Single material may be shared by multiple models.
    ModelMaterialMap    materials;
    // Objects are owned by a model. Each model may have multiple instances, each instance having its own transformation (shift, scale, rotation).
    ModelObjectPtrs     objects;
    
    // Default constructor assigns a new ID to the model.
    Model() {}
    ~Model() { this->clear_objects(); this->clear_materials(); }

    /* To be able to return an object from own copy / clone methods. Hopefully the compiler will do the "Copy elision" */
    /* (Omits copy and move(since C++11) constructors, resulting in zero - copy pass - by - value semantics). */
    Model(const Model &rhs) : ModelBase(-1) { this->assign_copy(rhs); }
    explicit Model(Model &&rhs) : ModelBase(-1) { this->assign_copy(std::move(rhs)); }
    Model& operator=(const Model &rhs) { this->assign_copy(rhs); return *this; }
    Model& operator=(Model &&rhs) { this->assign_copy(std::move(rhs)); return *this; }

    MODELBASE_DERIVED_COPY_MOVE_CLONE(Model)

    static Model read_from_file(const std::string &input_file, DynamicPrintConfig *config = nullptr, bool add_default_instances = true);
    static Model read_from_archive(const std::string &input_file, DynamicPrintConfig *config, bool add_default_instances = true);

    /// Repair the ModelObjects of the current Model.
    /// This function calls repair function on each TriangleMesh of each model object volume
    void         repair();

    // Add a new ModelObject to this Model, generate a new ID for this ModelObject.
    ModelObject* add_object();
    ModelObject* add_object(const char *name, const char *path, const TriangleMesh &mesh);
    ModelObject* add_object(const char *name, const char *path, TriangleMesh &&mesh);
    ModelObject* add_object(const ModelObject &other);
    void         delete_object(size_t idx);
    bool         delete_object(ModelID id);
    bool         delete_object(ModelObject* object);
    void         clear_objects();

    ModelMaterial* add_material(t_model_material_id material_id);
    ModelMaterial* add_material(t_model_material_id material_id, const ModelMaterial &other);
    ModelMaterial* get_material(t_model_material_id material_id) {
        ModelMaterialMap::iterator i = this->materials.find(material_id);
        return (i == this->materials.end()) ? nullptr : i->second;
    }

    void          delete_material(t_model_material_id material_id);
    void          clear_materials();
    bool          add_default_instances();
    // Returns approximate axis aligned bounding box of this model
    BoundingBoxf3 bounding_box() const;
    // Set the print_volume_state of PrintObject::instances, 
    // return total number of printable objects.
    unsigned int update_print_volume_state(const BoundingBoxf3 &print_volume);
	// Returns true if any ModelObject was modified.
    bool center_instances_around_point(const Vec2d &point);
    void translate(coordf_t x, coordf_t y, coordf_t z) { for (ModelObject *o : this->objects) o->translate(x, y, z); }
    TriangleMesh mesh() const;
    bool arrange_objects(coordf_t dist, const BoundingBoxf* bb = NULL);
    // Croaks if the duplicated objects do not fit the print bed.
    void duplicate(size_t copies_num, coordf_t dist, const BoundingBoxf* bb = NULL);
    void duplicate_objects(size_t copies_num, coordf_t dist, const BoundingBoxf* bb = NULL);
    void duplicate_objects_grid(size_t x, size_t y, coordf_t dist);

    bool looks_like_multipart_object() const;
    void convert_multipart_object(unsigned int max_extruders);

    // Ensures that the min z of the model is not negative
    void adjust_min_z();

    void print_info() const { for (const ModelObject *o : this->objects) o->print_info(); }

    static unsigned int get_auto_extruder_id(unsigned int max_extruders);
    static std::string get_auto_extruder_id_as_string(unsigned int max_extruders);
    static void reset_auto_extruder_id();

    // Propose an output file name & path based on the first printable object's name and source input file's path.
    std::string         propose_export_file_name_and_path() const;
    // Propose an output path, replace extension. The new_extension shall contain the initial dot.
    std::string         propose_export_file_name_and_path(const std::string &new_extension) const;

private:
    MODELBASE_DERIVED_PRIVATE_COPY_MOVE(Model)
};

#undef MODELBASE_DERIVED_COPY_MOVE_CLONE
#undef MODELBASE_DERIVED_PRIVATE_COPY_MOVE

// Test whether the two models contain the same number of ModelObjects with the same set of IDs
// ordered in the same order. In that case it is not necessary to kill the background processing.
extern bool model_object_list_equal(const Model &model_old, const Model &model_new);

// Test whether the new model is just an extension of the old model (new objects were added
// to the end of the original list. In that case it is not necessary to kill the background processing.
extern bool model_object_list_extended(const Model &model_old, const Model &model_new);

// Test whether the new ModelObject contains a different set of volumes (or sorted in a different order)
// than the old ModelObject.
extern bool model_volume_list_changed(const ModelObject &model_object_old, const ModelObject &model_object_new, const ModelVolumeType type);

#ifndef NDEBUG
// Verify whether the IDs of Model / ModelObject / ModelVolume / ModelInstance / ModelMaterial are valid and unique.
void check_model_ids_validity(const Model &model);
void check_model_ids_equal(const Model &model1, const Model &model2);
#endif /* NDEBUG */

}

#endif

#ifndef slic3r_GLGizmoBase_hpp_
#define slic3r_GLGizmoBase_hpp_

#include "libslic3r/Point.hpp"

#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/Selection.hpp"


class wxWindow;
class GLUquadric;
typedef class GLUquadric GLUquadricObj;


namespace Slic3r {

class BoundingBoxf3;
class Linef3;
class ModelObject;

namespace GUI {

static const float DEFAULT_BASE_COLOR[3] = { 0.625f, 0.625f, 0.625f };
static const float DEFAULT_DRAG_COLOR[3] = { 1.0f, 1.0f, 1.0f };
static const float DEFAULT_HIGHLIGHT_COLOR[3] = { 1.0f, 0.38f, 0.0f };
static const float AXES_COLOR[3][3] = { { 0.75f, 0.0f, 0.0f }, { 0.0f, 0.75f, 0.0f }, { 0.0f, 0.0f, 0.75f } };
static const float CONSTRAINED_COLOR[3] = { 0.5f, 0.5f, 0.5f };



class ImGuiWrapper;


class GLGizmoBase
{
public:
    // Starting value for ids to avoid clashing with ids used by GLVolumes
    // (254 is choosen to leave some space for forward compatibility)
    static const unsigned int BASE_ID = 255 * 255 * 254;

protected:
    struct Grabber
    {
        static const float SizeFactor;
        static const float MinHalfSize;
        static const float DraggingScaleFactor;

        Vec3d center;
        Vec3d angles;
        float color[3];
        bool enabled;
        bool dragging;

        Grabber();

        void render(bool hover, float size) const;
        void render_for_picking(float size) const { render(size, color, false); }

        float get_half_size(float size) const;
        float get_dragging_half_size(float size) const;

    private:
        void render(float size, const float* render_color, bool use_lighting) const;
        void render_face(float half_size) const;
    };

public:
    enum EState
    {
        Off,
        Hover,
        On,
        Num_States
    };

    struct UpdateData
    {
        const Linef3 mouse_ray;
        const Point* mouse_pos;

        UpdateData(const Linef3& mouse_ray, const Point* mouse_pos = nullptr)
            : mouse_ray(mouse_ray), mouse_pos(mouse_pos)
        {}
    };

protected:
    GLCanvas3D& m_parent;

    int m_group_id;
    EState m_state;
    int m_shortcut_key;
#if ENABLE_SVG_ICONS
    std::string m_icon_filename;
#endif // ENABLE_SVG_ICONS
    unsigned int m_sprite_id;
    int m_hover_id;
    bool m_dragging;
    float m_base_color[3];
    float m_drag_color[3];
    float m_highlight_color[3];
    mutable std::vector<Grabber> m_grabbers;
    ImGuiWrapper* m_imgui;

public:
#if ENABLE_SVG_ICONS
    GLGizmoBase(GLCanvas3D& parent, const std::string& icon_filename, unsigned int sprite_id);
#else
    GLGizmoBase(GLCanvas3D& parent, unsigned int sprite_id);
#endif // ENABLE_SVG_ICONS
    virtual ~GLGizmoBase() {}

    bool init() { return on_init(); }

    std::string get_name() const { return on_get_name(); }

    int get_group_id() const { return m_group_id; }
    void set_group_id(int id) { m_group_id = id; }

    EState get_state() const { return m_state; }
    void set_state(EState state) { m_state = state; on_set_state(); }

    int get_shortcut_key() const { return m_shortcut_key; }
    void set_shortcut_key(int key) { m_shortcut_key = key; }

#if ENABLE_SVG_ICONS
    const std::string& get_icon_filename() const { return m_icon_filename; }
#endif // ENABLE_SVG_ICONS

    bool is_activable(const Selection& selection) const { return on_is_activable(selection); }
    bool is_selectable() const { return on_is_selectable(); }

    unsigned int get_sprite_id() const { return m_sprite_id; }

    int get_hover_id() const { return m_hover_id; }
    void set_hover_id(int id);
    
    void set_highlight_color(const float* color);

    void enable_grabber(unsigned int id);
    void disable_grabber(unsigned int id);

    void start_dragging(const Selection& selection);
    void stop_dragging();

    bool is_dragging() const { return m_dragging; }

    void update(const UpdateData& data, const Selection& selection);

    void render(const Selection& selection) const { on_render(selection); }
    void render_for_picking(const Selection& selection) const { on_render_for_picking(selection); }
    void render_input_window(float x, float y, float bottom_limit, const Selection& selection) { on_render_input_window(x, y, bottom_limit, selection); }

protected:
    virtual bool on_init() = 0;
    virtual std::string on_get_name() const = 0;
    virtual void on_set_state() {}
    virtual void on_set_hover_id() {}
    virtual bool on_is_activable(const Selection& selection) const { return true; }
    virtual bool on_is_selectable() const { return true; }
    virtual void on_enable_grabber(unsigned int id) {}
    virtual void on_disable_grabber(unsigned int id) {}
    virtual void on_start_dragging(const Selection& selection) {}
    virtual void on_stop_dragging() {}
    virtual void on_update(const UpdateData& data, const Selection& selection) = 0;
    virtual void on_render(const Selection& selection) const = 0;
    virtual void on_render_for_picking(const Selection& selection) const = 0;
    virtual void on_render_input_window(float x, float y, float bottom_limit, const Selection& selection) {}

    // Returns the picking color for the given id, based on the BASE_ID constant
    // No check is made for clashing with other picking color (i.e. GLVolumes)
    std::array<float, 3> picking_color_component(unsigned int id) const;
    void render_grabbers(const BoundingBoxf3& box) const;
    void render_grabbers(float size) const;
    void render_grabbers_for_picking(const BoundingBoxf3& box) const;

    void set_tooltip(const std::string& tooltip) const;
    std::string format(float value, unsigned int decimals) const;
};

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_GLGizmoBase_hpp_

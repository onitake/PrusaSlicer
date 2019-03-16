#ifndef slic3r_ConfigWizard_private_hpp_
#define slic3r_ConfigWizard_private_hpp_

#include "ConfigWizard.hpp"

#include <vector>
#include <set>
#include <unordered_map>
#include <functional>
#include <boost/filesystem.hpp>

#include <wx/sizer.h>
#include <wx/panel.h>
#include <wx/button.h>
#include <wx/choice.h>
#include <wx/spinctrl.h>
#include <wx/textctrl.h>

#include "libslic3r/PrintConfig.hpp"
#include "slic3r/Utils/PresetUpdater.hpp"
#include "GUI.hpp"
#include "AppConfig.hpp"
#include "Preset.hpp"
#include "BedShapeDialog.hpp"

namespace fs = boost::filesystem;

namespace Slic3r {
namespace GUI {

enum {
    WRAP_WIDTH = 500,
    MIN_HEIGHT = 150,
    MODEL_MIN_WRAP = 150,

    DIALOG_MARGIN = 15,
    INDEX_MARGIN = 40,
    BTN_SPACING = 10,
    INDENT_SPACING = 30,
    VERTICAL_SPACING = 10,

    MAX_COLS = 4,
    ROW_SPACING = 75,
};

typedef std::function<bool(const VendorProfile::PrinterModel&)> ModelFilter;

struct PrinterPicker: wxPanel
{
    struct Checkbox : wxCheckBox
    {
        Checkbox(wxWindow *parent, const wxString &label, const std::string &model, const std::string &variant) :
            wxCheckBox(parent, wxID_ANY, label),
            model(model),
            variant(variant)
        {}

        std::string model;
        std::string variant;
    };

    const std::string vendor_id;
    std::vector<Checkbox*> cboxes;
    std::vector<Checkbox*> cboxes_alt;

    PrinterPicker(wxWindow *parent, const VendorProfile &vendor, wxString title, size_t max_cols, const AppConfig &appconfig_vendors, const ModelFilter &filter);
    PrinterPicker(wxWindow *parent, const VendorProfile &vendor, wxString title, size_t max_cols, const AppConfig &appconfig_vendors);

    void select_all(bool select, bool alternates = false);
    void select_one(size_t i, bool select);
    void on_checkbox(const Checkbox *cbox, bool checked);

    int get_width() const { return width; }
private:
    int width;
};

struct ConfigWizardPage: wxPanel
{
    ConfigWizard *parent;
    const wxString shortname;
    wxBoxSizer *content;
    const unsigned indent;

    ConfigWizardPage(ConfigWizard *parent, wxString title, wxString shortname, unsigned indent = 0);
    virtual ~ConfigWizardPage();

    template<class T>
    void append(T *thing, int proportion = 0, int flag = wxEXPAND|wxTOP|wxBOTTOM, int border = 10)
    {
        content->Add(thing, proportion, flag, border);
    }

    void append_text(wxString text);
    void append_spacer(int space);

    ConfigWizard::priv *wizard_p() const { return parent->p.get(); }

    virtual void apply_custom_config(DynamicPrintConfig &config) {}
};

struct PageWelcome: ConfigWizardPage
{
    wxCheckBox *cbox_reset;

    PageWelcome(ConfigWizard *parent);

    bool reset_user_profile() const { return cbox_reset != nullptr ? cbox_reset->GetValue() : false; }
};

struct PagePrinters: ConfigWizardPage
{
    std::vector<PrinterPicker *> printer_pickers;

    PagePrinters(ConfigWizard *parent, wxString title, wxString shortname, const VendorProfile &vendor, unsigned indent);

    void select_all(bool select, bool alternates = false);
    int get_width() const;
};

struct PageCustom: ConfigWizardPage
{
    PageCustom(ConfigWizard *parent);

    bool custom_wanted() const { return cb_custom->GetValue(); }
    std::string profile_name() const { return into_u8(tc_profile_name->GetValue()); }

private:
    static const char* default_profile_name;

    wxCheckBox *cb_custom;
    wxTextCtrl *tc_profile_name;
    wxString profile_name_prev;
};

struct PageUpdate: ConfigWizardPage
{
    bool version_check;
    bool preset_update;

    PageUpdate(ConfigWizard *parent);
};

struct PageVendors: ConfigWizardPage
{
    std::vector<PrinterPicker*> pickers;

    PageVendors(ConfigWizard *parent);

    void on_vendor_pick(size_t i);
};

struct PageFirmware: ConfigWizardPage
{
    const ConfigOptionDef &gcode_opt;
    wxChoice *gcode_picker;

    PageFirmware(ConfigWizard *parent);
    virtual void apply_custom_config(DynamicPrintConfig &config);
};

struct PageBedShape: ConfigWizardPage
{
    BedShapePanel *shape_panel;

    PageBedShape(ConfigWizard *parent);
    virtual void apply_custom_config(DynamicPrintConfig &config);
};

struct PageDiameters: ConfigWizardPage
{
    wxSpinCtrlDouble *spin_nozzle;
    wxSpinCtrlDouble *spin_filam;

    PageDiameters(ConfigWizard *parent);
    virtual void apply_custom_config(DynamicPrintConfig &config);
};

struct PageTemperatures: ConfigWizardPage
{
    wxSpinCtrlDouble *spin_extr;
    wxSpinCtrlDouble *spin_bed;

    PageTemperatures(ConfigWizard *parent);
    virtual void apply_custom_config(DynamicPrintConfig &config);
};


class ConfigWizardIndex: public wxPanel
{
public:
    ConfigWizardIndex(wxWindow *parent);

    void add_page(ConfigWizardPage *page);
    void add_label(wxString label, unsigned indent = 0);

    size_t active_item() const { return item_active; }
    ConfigWizardPage* active_page() const;
    bool active_is_last() const { return item_active < items.size() && item_active == last_page; }

    void go_prev();
    void go_next();
    void go_to(size_t i);
    void go_to(ConfigWizardPage *page);

    void clear();

private:
    struct Item
    {
        wxString label;
        unsigned indent;
        ConfigWizardPage *page;     // nullptr page => label-only item

        bool operator==(ConfigWizardPage *page) const { return this->page == page; }
    };

    int em;
    int em_h;

    const wxBitmap bg;
    const wxBitmap bullet_black;
    const wxBitmap bullet_blue;
    const wxBitmap bullet_white;

    std::vector<Item> items;
    size_t item_active;
    ssize_t item_hover;
    size_t last_page;

    int item_height() const { return std::max(bullet_black.GetSize().GetHeight(), em) + em; }

    void on_paint(wxPaintEvent &evt);
    void on_mouse_move(wxMouseEvent &evt);
};

wxDEFINE_EVENT(EVT_INDEX_PAGE, wxCommandEvent);

struct ConfigWizard::priv
{
    ConfigWizard *q;
    ConfigWizard::RunReason run_reason;
    AppConfig appconfig_vendors;
    std::unordered_map<std::string, VendorProfile> vendors;
    std::unordered_map<std::string, std::string> vendors_rsrc;
    std::unique_ptr<DynamicPrintConfig> custom_config;

    wxScrolledWindow *hscroll = nullptr;
    wxBoxSizer *hscroll_sizer = nullptr;
    wxBoxSizer *btnsizer = nullptr;
    ConfigWizardPage *page_current = nullptr;
    ConfigWizardIndex *index = nullptr;
    wxButton *btn_prev = nullptr;
    wxButton *btn_next = nullptr;
    wxButton *btn_finish = nullptr;
    wxButton *btn_cancel = nullptr;

    PageWelcome      *page_welcome = nullptr;
    PagePrinters     *page_fff = nullptr;
    PageCustom       *page_custom = nullptr;
    PageUpdate       *page_update = nullptr;
    PageVendors      *page_vendors = nullptr;   // XXX: ?

    // Custom setup pages
    PageFirmware     *page_firmware = nullptr;
    PageBedShape     *page_bed = nullptr;
    PageDiameters    *page_diams = nullptr;
    PageTemperatures *page_temps = nullptr;

    priv(ConfigWizard *q) : q(q) {}

    void load_pages(bool custom_setup);

    bool check_first_variant() const;
    void load_vendors();
    void add_page(ConfigWizardPage *page);
    void enable_next(bool enable);

    void on_custom_setup(bool custom_wanted);

    void apply_config(AppConfig *app_config, PresetBundle *preset_bundle, const PresetUpdater *updater);
};



}
}

#endif

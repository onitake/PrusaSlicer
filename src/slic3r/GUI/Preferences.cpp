#include "Preferences.hpp"
#include "AppConfig.hpp"
#include "OptionsGroup.hpp"
#include "I18N.hpp"

namespace Slic3r {
namespace GUI {

PreferencesDialog::PreferencesDialog(wxWindow* parent) : 
    DPIDialog(parent, wxID_ANY, _(L("Preferences")), wxDefaultPosition, 
              wxDefaultSize, wxDEFAULT_DIALOG_STYLE)
{
	build();
}

void PreferencesDialog::build()
{
	auto app_config = get_app_config();
	m_optgroup = std::make_shared<ConfigOptionsGroup>(this, _(L("General")));
    m_optgroup->label_width = 40;
	m_optgroup->m_on_change = [this](t_config_option_key opt_key, boost::any value){
		m_values[opt_key] = boost::any_cast<bool>(value) ? "1" : "0";
	};

	// TODO
//    $optgroup->append_single_option_line(Slic3r::GUI::OptionsGroup::Option->new(
//        opt_id = > 'version_check',
//        type = > 'bool',
//        label = > 'Check for updates',
//        tooltip = > 'If this is enabled, Slic3r will check for updates daily and display a reminder if a newer version is available.',
//        default = > $app_config->get("version_check") // 1,
//        readonly = > !wxTheApp->have_version_check,
//    ));

	ConfigOptionDef def;
	def.label = L("Remember output directory");
	def.type = coBool;
	def.tooltip = L("If this is enabled, Slic3r will prompt the last output directory "
					  "instead of the one containing the input files.");
    def.set_default_value(new ConfigOptionBool{ app_config->has("remember_output_path") ? app_config->get("remember_output_path") == "1" : true });
    Option option(def, "remember_output_path");
	m_optgroup->append_single_option_line(option);

	def.label = L("Auto-center parts");
	def.type = coBool;
	def.tooltip = L("If this is enabled, Slic3r will auto-center objects "
					  "around the print bed center.");
	def.set_default_value(new ConfigOptionBool{ app_config->get("autocenter") == "1" });
	option = Option (def,"autocenter");
	m_optgroup->append_single_option_line(option);

	def.label = L("Background processing");
	def.type = coBool;
	def.tooltip = L("If this is enabled, Slic3r will pre-process objects as soon "
					  "as they\'re loaded in order to save time when exporting G-code.");
	def.set_default_value(new ConfigOptionBool{ app_config->get("background_processing") == "1" });
	option = Option (def,"background_processing");
	m_optgroup->append_single_option_line(option);

	// Please keep in sync with ConfigWizard
	def.label = L("Check for application updates");
	def.type = coBool;
	def.tooltip = L("If enabled, PrusaSlicer will check for the new versions of itself online. When a new version becomes available a notification is displayed at the next application startup (never during program usage). This is only a notification mechanisms, no automatic installation is done.");
	def.set_default_value(new ConfigOptionBool(app_config->get("version_check") == "1"));
	option = Option (def, "version_check");
	m_optgroup->append_single_option_line(option);

	// Please keep in sync with ConfigWizard
	def.label = L("Update built-in Presets automatically");
	def.type = coBool;
	def.tooltip = L("If enabled, Slic3r downloads updates of built-in system presets in the background. These updates are downloaded into a separate temporary location. When a new preset version becomes available it is offered at application startup.");
	def.set_default_value(new ConfigOptionBool(app_config->get("preset_update") == "1"));
	option = Option (def, "preset_update");
	m_optgroup->append_single_option_line(option);

	def.label = L("Suppress \" - default - \" presets");
	def.type = coBool;
	def.tooltip = L("Suppress \" - default - \" presets in the Print / Filament / Printer "
					  "selections once there are any other valid presets available.");
	def.set_default_value(new ConfigOptionBool{ app_config->get("no_defaults") == "1" });
	option = Option (def,"no_defaults");
	m_optgroup->append_single_option_line(option);

	def.label = L("Show incompatible print and filament presets");
	def.type = coBool;
	def.tooltip = L("When checked, the print and filament presets are shown in the preset editor "
					  "even if they are marked as incompatible with the active printer");
	def.set_default_value(new ConfigOptionBool{ app_config->get("show_incompatible_presets") == "1" });
	option = Option (def,"show_incompatible_presets");
	m_optgroup->append_single_option_line(option);

	// TODO: remove?
	def.label = L("Use legacy OpenGL 1.1 rendering");
	def.type = coBool;
	def.tooltip = L("If you have rendering issues caused by a buggy OpenGL 2.0 driver, "
					  "you may try to check this checkbox. This will disable the layer height "
					  "editing and anti aliasing, so it is likely better to upgrade your graphics driver.");
	def.set_default_value(new ConfigOptionBool{ app_config->get("use_legacy_opengl") == "1" });
	option = Option (def,"use_legacy_opengl");
	m_optgroup->append_single_option_line(option);

#if __APPLE__
	def.label = L("Use Retina resolution for the 3D scene");
	def.type = coBool;
	def.tooltip = L("If enabled, the 3D scene will be rendered in Retina resolution. "
	                "If you are experiencing 3D performance problems, disabling this option may help.");
	def.set_default_value(new ConfigOptionBool{ app_config->get("use_retina_opengl") == "1" });
	option = Option (def, "use_retina_opengl");
	m_optgroup->append_single_option_line(option);
#endif

	auto sizer = new wxBoxSizer(wxVERTICAL);
	sizer->Add(m_optgroup->sizer, 0, wxEXPAND | wxBOTTOM | wxLEFT | wxRIGHT, 10);

    SetFont(wxGetApp().normal_font());

	auto buttons = CreateStdDialogButtonSizer(wxOK | wxCANCEL);
	wxButton* btn = static_cast<wxButton*>(FindWindowById(wxID_OK, this));
	btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { accept(); });
	sizer->Add(buttons, 0, wxALIGN_CENTER_HORIZONTAL | wxBOTTOM, 10);

	SetSizer(sizer);
	sizer->SetSizeHints(this);
}

void PreferencesDialog::accept()
{
	if (m_values.find("no_defaults")       != m_values.end() ||
		m_values.find("use_legacy_opengl") != m_values.end()) {
        warning_catcher(this, wxString::Format(_(L("You need to restart %s to make the changes effective.")), SLIC3R_APP_NAME));
	}

	auto app_config = get_app_config();
	for (std::map<std::string, std::string>::iterator it = m_values.begin(); it != m_values.end(); ++it) {
		app_config->set(it->first, it->second);
	}

	EndModal(wxID_OK);

	// Nothify the UI to update itself from the ini file.
    wxGetApp().update_ui_from_settings();
}

void PreferencesDialog::on_dpi_changed(const wxRect &suggested_rect)
{
    m_optgroup->msw_rescale();

    const int em = em_unit();

    msw_buttons_rescale(this, em, { wxID_OK, wxID_CANCEL });

    const wxSize& size = wxSize(47 * em, 28 * em);

    SetMinSize(size);
    Fit();

    Refresh();
}

} // GUI
} // Slic3r
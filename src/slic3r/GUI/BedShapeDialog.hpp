#ifndef slic3r_BedShapeDialog_hpp_
#define slic3r_BedShapeDialog_hpp_
// The bed shape dialog.
// The dialog opens from Print Settins tab->Bed Shape : Set...

#include "OptionsGroup.hpp"
#include "2DBed.hpp"
#include "I18N.hpp"

#include <wx/dialog.h>
#include <wx/choicebk.h>

namespace Slic3r {
namespace GUI {

using ConfigOptionsGroupShp = std::shared_ptr<ConfigOptionsGroup>;
class BedShapePanel : public wxPanel
{
	Bed_2D*			m_canvas;

public:
	BedShapePanel(wxWindow* parent) : wxPanel(parent, wxID_ANY) {}
	~BedShapePanel() {}

	void		build_panel(ConfigOptionPoints* default_pt);
	
	ConfigOptionsGroupShp	init_shape_options_page(wxString title);
	void		set_shape(ConfigOptionPoints* points);
	void		update_preview();
	void		update_shape();
	void		load_stl();
	
	// Returns the resulting bed shape polygon. This value will be stored to the ini file.
	std::vector<Vec2d>	GetValue() { return m_canvas->m_bed_shape; }

	wxChoicebook*	m_shape_options_book;
	std::vector <ConfigOptionsGroupShp>	m_optgroups;

};

class BedShapeDialog : public DPIDialog
{
	BedShapePanel*	m_panel;
public:
	BedShapeDialog(wxWindow* parent) : DPIDialog(parent, wxID_ANY, _(L("Bed Shape")),
        wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER) {}
	~BedShapeDialog() {}

	void		build_dialog(ConfigOptionPoints* default_pt);
	std::vector<Vec2d>	GetValue() { return m_panel->GetValue(); }

protected:
    void on_dpi_changed(const wxRect &suggested_rect) override;
};

} // GUI
} // Slic3r


#endif  /* slic3r_BedShapeDialog_hpp_ */

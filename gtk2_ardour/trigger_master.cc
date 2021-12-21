/*
 * Copyright (C) 2021 Paul Davis <paul@linuxaudiosystems.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include <gtkmm/filechooserdialog.h>
#include <gtkmm/menu.h>
#include <gtkmm/menuitem.h>
#include <gtkmm/stock.h>

#include "pbd/compose.h"
#include "pbd/convert.h"

#include "ardour/region.h"

#include "canvas/polygon.h"
#include "canvas/text.h"

#include "gtkmm2ext/actions.h"
#include "gtkmm2ext/colors.h"
#include "gtkmm2ext/utils.h"

#include "ardour_ui.h"
#include "gui_thread.h"
#include "public_editor.h"
#include "region_view.h"
#include "selection.h"
#include "timers.h"
#include "trigger_master.h"
#include "trigger_ui.h"
#include "ui_config.h"
#include "utils.h"

#include "pbd/i18n.h"

using namespace ARDOUR;
using namespace ArdourCanvas;
using namespace Gtkmm2ext;
using namespace PBD;

static const int nslices = 8; // in 8 pie slices .. TODO .. maybe make this meter-senstive ... triplets and such... ?

Loopster::Loopster (Item* parent)
	: ArdourCanvas::Rectangle (parent)
	, _fraction (0)
{
}

void
Loopster::set_fraction (float f)
{
	f = std::max (0.f, f);
	f = std::min (1.f, f);

	float prior_slice = floor (_fraction * nslices);
	float new_slice   = floor (f * nslices);

	if (new_slice != prior_slice) {
		_fraction = f;
		redraw ();
	}
}

void
Loopster::render (ArdourCanvas::Rect const& area, Cairo::RefPtr<Cairo::Context> context) const
{
	/* Note that item_to_window() already takes _position into account (as
	 * part of item_to_canvas()
	 */
	ArdourCanvas::Rect       self (item_to_window (_rect));
	ArdourCanvas::Rect const draw = self.intersection (area);

	if (!draw) {
		return;
	}

	context->set_identity_matrix ();
	context->translate (self.x0, self.y0 - 0.5);

	float size = _rect.height ();

	const double scale = UIConfiguration::instance ().get_ui_scale ();

	/* white area */
	set_source_rgba (context, rgba_to_color (1, 1, 1, 1));
	context->arc (size / 2, size / 2, size / 2 - 4 * scale, 0, 2 * M_PI);
	context->fill ();

	/* arc fill */
	context->set_line_width (5 * scale);
	float slices        = floor (_fraction * nslices);
	float deg_per_slice = 360 / nslices;
	float degrees       = slices * deg_per_slice;
	float radians       = (degrees / 180) * M_PI;
	set_source_rgba (context, rgba_to_color (0, 0, 0, 1));
	context->arc (size / 2, size / 2, size / 2 - 5 * scale, 1.5 * M_PI + radians, 1.5 * M_PI + 2 * M_PI);
	context->stroke ();

	context->set_line_width (1);
	context->set_identity_matrix ();
}

class PassThru : public ArdourCanvas::Rectangle
{
public:
	PassThru (ArdourCanvas::Item* canvas);

	void render (ArdourCanvas::Rect const& area, Cairo::RefPtr<Cairo::Context> context) const;
	void set_enabled (bool e);

private:
	bool _enabled;
};

PassThru::PassThru (Item* parent)
	: ArdourCanvas::Rectangle (parent)
{
	set_enabled (false);
}

void
PassThru::set_enabled (bool e)
{
	if (e != _enabled) {
		_enabled = e;
		redraw ();
	}
}

void
PassThru::render (ArdourCanvas::Rect const& area, Cairo::RefPtr<Cairo::Context> context) const
{
	/* Note that item_to_window() already takes _position into account (as
	 * part of item_to_canvas()
	 */
	ArdourCanvas::Rect       self (item_to_window (_rect));
	ArdourCanvas::Rect const draw = self.intersection (area);

	if (!draw) {
		return;
	}

	context->set_identity_matrix ();
	context->translate (self.x0, self.y0 - 0.5);

	float size = _rect.height ();

	const double scale = UIConfiguration::instance ().get_ui_scale ();

	if (_enabled) {
		/* outer white circle */
		set_source_rgba (context, rgba_to_color (1, 1, 1, 1));
		context->arc (size / 2, size / 2, size / 2 - 3 * scale, 0, 2 * M_PI);
		context->fill ();

		/* black circle */
		set_source_rgba (context, rgba_to_color (0, 0, 0, 1));
		context->arc (size / 2, size / 2, size / 2 - 5 * scale, 0, 2 * M_PI);
		context->fill ();

		/* inner white circle */
		set_source_rgba (context, rgba_to_color (1, 1, 1, 1));
		context->arc (size / 2, size / 2, size / 2 - 7 * scale, 0, 2 * M_PI);
		context->fill ();
	}

	context->set_identity_matrix ();
}

TriggerMaster::TriggerMaster (Item* parent)
	: ArdourCanvas::Rectangle (parent)
	, _context_menu (0)
	, _ignore_menu_action (false)
{
	set_layout_sensitive (true); // why???

	name = X_("trigger stopper");

	Event.connect (sigc::mem_fun (*this, &TriggerMaster::event_handler));

	stop_shape = new ArdourCanvas::Polygon (this);
	stop_shape->set_outline (true);
	stop_shape->set_fill (false);
	stop_shape->name = X_("stopbutton");
	stop_shape->set_ignore_events (true);
	stop_shape->show ();

	name_text = new Text (this);
	name_text->set ("");
	name_text->set_ignore_events (false);

	_loopster = new Loopster (this);
	_passthru = new PassThru (this);

#if 0 /* XXX trigger changes */
	_triggerbox->PropertyChanged.connect (_trigger_prop_connection, MISSING_INVALIDATOR, boost::bind (&TriggerMaster::prop_change, this, _1), gui_context());
	PropertyChange changed;
	changed.add (ARDOUR::Properties::name);
	changed.add (ARDOUR::Properties::running);
	prop_change (changed);
#endif

#if 0 /* XXX route changes */
	dynamic_cast<Stripable*> (_triggerbox->owner())->presentation_info().Change.connect (_owner_prop_connection, MISSING_INVALIDATOR, boost::bind (&TriggerMaster::owner_prop_change, this, _1), gui_context());
#endif

	_update_connection = Timers::rapid_connect (sigc::mem_fun (*this, &TriggerMaster::maybe_update));

	/* prefs (theme colors) */
	UIConfiguration::instance ().ParameterChanged.connect (sigc::mem_fun (*this, &TriggerMaster::ui_parameter_changed));
	set_default_colors ();
}

TriggerMaster::~TriggerMaster ()
{
	_update_connection.disconnect ();
}

void
TriggerMaster::set_trigger (boost::shared_ptr<ARDOUR::TriggerBox> t)
{
	_triggerbox = t;
}

void
TriggerMaster::render (ArdourCanvas::Rect const& area, Cairo::RefPtr<Cairo::Context> context) const
{
	/* Note that item_to_window() already takes _position into account (as
	 * part of item_to_canvas()
	 */
	ArdourCanvas::Rect       self (item_to_window (_rect));
	ArdourCanvas::Rect const draw = self.intersection (area);

	if (!draw) {
		return;
	}

	float width  = _rect.width ();
	float height = _rect.height ();

	const double scale = UIConfiguration::instance ().get_ui_scale ();

	if (_fill && !_transparent) {
		setup_fill_context (context);
		context->rectangle (draw.x0, draw.y0, draw.width (), draw.height ());
		context->fill ();
	}

	render_children (area, context);

	if (true) {
		/* drop-shadow at top */
		Cairo::RefPtr<Cairo::LinearGradient> drop_shadow_pattern = Cairo::LinearGradient::create (0.0, 0.0, 0.0, 6 * scale);
		drop_shadow_pattern->add_color_stop_rgba (0, 0, 0, 0, 0.7);
		drop_shadow_pattern->add_color_stop_rgba (1, 0, 0, 0, 0.0);
		context->set_source (drop_shadow_pattern);
		context->rectangle (0, 0, width, 6 * scale);
		context->fill ();
	} else {
		/* line at top */
		context->set_identity_matrix ();
		context->translate (self.x0, self.y0 - 0.5);
		set_source_rgba (context, rgba_to_color (0, 0, 0, 1));
		context->rectangle (0, 0, width, 1.);
		context->fill ();
		context->set_identity_matrix ();
	}
}

void
TriggerMaster::owner_prop_change (PropertyChange const& pc)
{
	if (pc.contains (Properties::color)) {
	}
}

void
TriggerMaster::selection_change ()
{
}

bool
TriggerMaster::event_handler (GdkEvent* ev)
{
	if (!_triggerbox) {
		return false;
	}

	switch (ev->type) {
		case GDK_BUTTON_PRESS:
			if (ev->button.button == 1) {
				_triggerbox->request_stop_all ();
				return true;
			}
			break;
		case GDK_ENTER_NOTIFY:
			if (ev->crossing.detail != GDK_NOTIFY_INFERIOR) {
				name_text->set_color (UIConfiguration::instance ().color ("neutral:foregroundest"));
				stop_shape->set_outline_color (UIConfiguration::instance ().color ("neutral:foreground"));
				set_fill_color (HSV (fill_color ()).lighter (0.15).color ());
			}
			redraw ();
			break;
		case GDK_LEAVE_NOTIFY:
			if (ev->crossing.detail != GDK_NOTIFY_INFERIOR) {
				set_default_colors ();
			}
			redraw ();
			break;
		case GDK_BUTTON_RELEASE:
			switch (ev->button.button) {
				case 3:
					context_menu ();
					return true;
			}
		default:
			break;
	}

	return false;
}

void
TriggerMaster::context_menu ()
{
	using namespace Gtk;
	using namespace Gtk::Menu_Helpers;
	using namespace Temporal;

	delete _context_menu;

	_context_menu   = new Menu;
	MenuList& items = _context_menu->items ();
	_context_menu->set_name ("ArdourContextMenu");

	Menu*     follow_menu = manage (new Menu);
	MenuList& fitems      = follow_menu->items ();

	fitems.push_back (MenuElem (_("Stop"), sigc::bind (sigc::mem_fun (*this, &TriggerMaster::set_all_follow_action), Trigger::Stop)));
	fitems.push_back (MenuElem (_("Again"), sigc::bind (sigc::mem_fun (*this, &TriggerMaster::set_all_follow_action), Trigger::Again)));
	fitems.push_back (MenuElem (_("Next"), sigc::bind (sigc::mem_fun (*this, &TriggerMaster::set_all_follow_action), Trigger::NextTrigger)));
	fitems.push_back (MenuElem (_("Previous"), sigc::bind (sigc::mem_fun (*this, &TriggerMaster::set_all_follow_action), Trigger::PrevTrigger)));
	fitems.push_back (MenuElem (_("Any"), sigc::bind (sigc::mem_fun (*this, &TriggerMaster::set_all_follow_action), Trigger::AnyTrigger)));
	fitems.push_back (MenuElem (_("Other"), sigc::bind (sigc::mem_fun (*this, &TriggerMaster::set_all_follow_action), Trigger::OtherTrigger)));

	Menu*     launch_menu = manage (new Menu);
	MenuList& litems      = launch_menu->items ();

	litems.push_back (MenuElem (_("One Shot"), sigc::bind (sigc::mem_fun (*this, &TriggerMaster::set_all_launch_style), Trigger::OneShot)));
	litems.push_back (MenuElem (_("Gate"), sigc::bind (sigc::mem_fun (*this, &TriggerMaster::set_all_launch_style), Trigger::Gate)));
	litems.push_back (MenuElem (_("Toggle"), sigc::bind (sigc::mem_fun (*this, &TriggerMaster::set_all_launch_style), Trigger::Toggle)));
	litems.push_back (MenuElem (_("Repeat"), sigc::bind (sigc::mem_fun (*this, &TriggerMaster::set_all_launch_style), Trigger::Repeat)));

	Menu*     quant_menu = manage (new Menu);
	MenuList& qitems     = quant_menu->items ();

	BBT_Offset b;

	b = BBT_Offset (1, 0, 0);
	qitems.push_back (MenuElem (_("Global"), sigc::bind (sigc::mem_fun (*this, &TriggerMaster::set_all_quantization), b))); // TODO

	b = BBT_Offset (1, 0, 0);
	qitems.push_back (MenuElem (_("Bars"), sigc::bind (sigc::mem_fun (*this, &TriggerMaster::set_all_quantization), b)));
	b = BBT_Offset (0, 4, 0);
	qitems.push_back (MenuElem (_("Whole"), sigc::bind (sigc::mem_fun (*this, &TriggerMaster::set_all_quantization), b)));
	b = BBT_Offset (0, 2, 0);
	qitems.push_back (MenuElem (_("Half"), sigc::bind (sigc::mem_fun (*this, &TriggerMaster::set_all_quantization), b)));
	b = BBT_Offset (0, 1, 0);
	qitems.push_back (MenuElem (_("Quarters"), sigc::bind (sigc::mem_fun (*this, &TriggerMaster::set_all_quantization), b)));
	b = BBT_Offset (0, 0, ticks_per_beat / 2);
	qitems.push_back (MenuElem (_("Eighths"), sigc::bind (sigc::mem_fun (*this, &TriggerMaster::set_all_quantization), b)));
	b = BBT_Offset (0, 0, ticks_per_beat / 4);
	qitems.push_back (MenuElem (_("Sixteenths"), sigc::bind (sigc::mem_fun (*this, &TriggerMaster::set_all_quantization), b)));
	b = BBT_Offset (0, 0, ticks_per_beat / 8);
	qitems.push_back (MenuElem (_("Thirty-Seconds"), sigc::bind (sigc::mem_fun (*this, &TriggerMaster::set_all_quantization), b)));
	b = BBT_Offset (0, 0, ticks_per_beat / 16);
	qitems.push_back (MenuElem (_("Sixty-Fourths"), sigc::bind (sigc::mem_fun (*this, &TriggerMaster::set_all_quantization), b)));

	Menu*     load_menu = manage (new Menu);
	MenuList& loitems (load_menu->items ());

	items.push_back (CheckMenuElem (_("Toggle Monitor Thru"), sigc::mem_fun (*this, &TriggerMaster::toggle_thru)));
	if (_triggerbox->pass_thru ()) {
		_ignore_menu_action = true;
		dynamic_cast<Gtk::CheckMenuItem*> (&items.back ())->set_active (true);
		_ignore_menu_action = false;
	}

	items.push_back (MenuElem (_("Enable/Disable..."), sigc::mem_fun (*this, &TriggerMaster::maybe_update))); // TODO
	items.push_back (MenuElem (_("Follow Action..."), *follow_menu));
	items.push_back (MenuElem (_("Launch Style..."), *launch_menu));
	items.push_back (MenuElem (_("Quantization..."), *quant_menu));
	items.push_back (MenuElem (_("Clear All..."), sigc::mem_fun (*this, &TriggerMaster::maybe_update))); // TODO

	_context_menu->popup (1, gtk_get_current_event_time ());
}

void
TriggerMaster::toggle_thru ()
{
	if (_ignore_menu_action) {
		return;
	}

	_triggerbox->set_pass_thru (!_triggerbox->pass_thru ());
}

void
TriggerMaster::set_all_follow_action (Trigger::FollowAction fa)
{
	// TODO
}

void
TriggerMaster::set_all_launch_style (Trigger::LaunchStyle ls)
{
	// TODO
}

void
TriggerMaster::set_all_quantization (Temporal::BBT_Offset const& q)
{
	// TODO
}

void
TriggerMaster::maybe_update ()
{
	PropertyChange changed;
	changed.add (ARDOUR::Properties::name);
	changed.add (ARDOUR::Properties::running);
	prop_change (changed);
}

void
TriggerMaster::_size_allocate (ArdourCanvas::Rect const& alloc)
{
	Rectangle::_size_allocate (alloc);

	const double scale = UIConfiguration::instance ().get_ui_scale ();
	_poly_margin       = 3. * scale;

	const Distance width  = _rect.width ();
	const Distance height = _rect.height ();

	_poly_size = height - (_poly_margin * 2);

	Points p;
	p.push_back (Duple (_poly_margin, _poly_margin));
	p.push_back (Duple (_poly_margin, _poly_size));
	p.push_back (Duple (_poly_size, _poly_size));
	p.push_back (Duple (_poly_size, _poly_margin));
	stop_shape->set (p);

	float tleft  = _poly_size + (_poly_margin * 3);
	float twidth = width - _poly_size - (_poly_margin * 3);

	ArdourCanvas::Rect text_alloc (tleft, 0, twidth, height); // testing
	name_text->size_allocate (text_alloc);
	name_text->set_position (Duple (tleft, 1. * scale));
	name_text->clamp_width (twidth);

	_loopster->set (ArdourCanvas::Rect (0, 0, height, height));
	_passthru->set (ArdourCanvas::Rect (width - height, 0, width, height));

	/* font scale may have changed. uiconfig 'embeds' the ui-scale in the font */
	name_text->set_font_description (UIConfiguration::instance ().get_NormalFont ());
}

void
TriggerMaster::prop_change (PropertyChange const& change)
{
	if (!_triggerbox) {
		return;
	}

	_passthru->set_enabled (_triggerbox->pass_thru ());

	std::string text;

	ARDOUR::TriggerPtr trigger = _triggerbox->currently_playing ();
	if (!trigger) {
		name_text->set (text);
		_loopster->hide ();
		stop_shape->show ();
		return;
	}

	text = string_compose ("%1", (char)('A' + trigger->index ()));

	if (trigger->follow_count () > 1) {
		text.append (string_compose (X_(" %1/%2"), trigger->loop_count ()+1, trigger->follow_count ()));
	}

	name_text->set (text);

	if (trigger->active ()) {
		double f = trigger->position_as_fraction ();
		_loopster->set_fraction (f);
		_loopster->show ();
		stop_shape->hide ();
	} else {
		_loopster->hide ();
		stop_shape->show ();
	}
}

void
TriggerMaster::set_default_colors ()
{
	set_fill_color (HSV (UIConfiguration::instance ().color ("theme:bg")).darker (0.25).color ());
	name_text->set_color (UIConfiguration::instance ().color ("neutral:foreground"));
	stop_shape->set_outline_color (UIConfiguration::instance ().color ("neutral:midground"));
}

void
TriggerMaster::ui_parameter_changed (std::string const& p)
{
	if (p == "color-file") {
		set_default_colors ();
	}
}

CueMaster::CueMaster (Item* parent)
	: ArdourCanvas::Rectangle (parent)
{
	set_layout_sensitive (true); // why???

	name = X_("trigger stopper");

	Event.connect (sigc::mem_fun (*this, &CueMaster::event_handler));

	stop_shape = new ArdourCanvas::Polygon (this);
	stop_shape->set_outline (true);
	stop_shape->set_fill (false);
	stop_shape->name = X_("stopbutton");
	stop_shape->set_ignore_events (true);
	stop_shape->show ();

	name_text = new Text (this);
	name_text->set ("");
	name_text->set_ignore_events (false);

	/* prefs (theme colors) */
	UIConfiguration::instance ().ParameterChanged.connect (sigc::mem_fun (*this, &CueMaster::ui_parameter_changed));
	set_default_colors ();
}

CueMaster::~CueMaster ()
{
}

void
CueMaster::render (ArdourCanvas::Rect const& area, Cairo::RefPtr<Cairo::Context> context) const
{
	/* Note that item_to_window() already takes _position into account (as
	   part of item_to_canvas()
	*/
	ArdourCanvas::Rect       self (item_to_window (_rect));
	ArdourCanvas::Rect const draw = self.intersection (area);

	if (!draw) {
		return;
	}

	float width  = _rect.width ();
	float height = _rect.height ();

	const double scale = UIConfiguration::instance ().get_ui_scale ();

	if (_fill && !_transparent) {
		setup_fill_context (context);
		context->rectangle (draw.x0, draw.y0, draw.width (), draw.height ());
		context->fill ();
	}

	render_children (area, context);

	if (true) {
		/* drop-shadow at top */
		Cairo::RefPtr<Cairo::LinearGradient> drop_shadow_pattern = Cairo::LinearGradient::create (0.0, 0.0, 0.0, 6 * scale);
		drop_shadow_pattern->add_color_stop_rgba (0, 0, 0, 0, 0.7);
		drop_shadow_pattern->add_color_stop_rgba (1, 0, 0, 0, 0.0);
		context->set_source (drop_shadow_pattern);
		context->rectangle (0, 0, width, 6 * scale);
		context->fill ();
	} else {
		/* line at top */
		context->set_identity_matrix ();
		context->translate (self.x0, self.y0 - 0.5);
		set_source_rgba (context, rgba_to_color (0, 0, 0, 1));
		context->rectangle (0, 0, width, 1.);
		context->fill ();
		context->set_identity_matrix ();
	}
}

bool
CueMaster::event_handler (GdkEvent* ev)
{
	switch (ev->type) {
		case GDK_BUTTON_PRESS:
			if (ev->button.button == 1) {
				_session->stop_all_triggers ();
				return true;
			}
			break;
		case GDK_ENTER_NOTIFY:
			if (ev->crossing.detail != GDK_NOTIFY_INFERIOR) {
				name_text->set_color (UIConfiguration::instance ().color ("neutral:foregroundest"));
				stop_shape->set_outline_color (UIConfiguration::instance ().color ("neutral:foreground"));
				set_fill_color (HSV (fill_color ()).lighter (0.15).color ());
			}
			break;
		case GDK_LEAVE_NOTIFY:
			if (ev->crossing.detail != GDK_NOTIFY_INFERIOR) {
				set_default_colors ();
			}
			break;
		default:
			break;
	}

	return false;
}

void
CueMaster::maybe_update ()
{
}

void
CueMaster::_size_allocate (ArdourCanvas::Rect const& alloc)
{
	Rectangle::_size_allocate (alloc);

	const double scale = UIConfiguration::instance ().get_ui_scale ();
	_poly_margin       = 2. * scale;

	const Distance width  = _rect.width ();
	const Distance height = _rect.height ();

	_poly_size = height - (_poly_margin * 2);

	float centering_offset = (width / 2) - _poly_margin - _poly_size / 2;

	Points p;
	p.push_back (Duple (centering_offset + _poly_margin, _poly_margin));
	p.push_back (Duple (centering_offset + _poly_margin, _poly_size));
	p.push_back (Duple (centering_offset + _poly_size, _poly_size));
	p.push_back (Duple (centering_offset + _poly_size, _poly_margin));
	stop_shape->set (p);

	float tleft  = _poly_size + (_poly_margin * 3);
	float twidth = width - _poly_size - (_poly_margin * 3);

	ArdourCanvas::Rect text_alloc (tleft, 0, twidth, height); // testing
	name_text->size_allocate (text_alloc);
	name_text->set_position (Duple (tleft, 1. * scale));
	name_text->clamp_width (twidth);

	/* font scale may have changed. uiconfig 'embeds' the ui-scale in the font */
	name_text->set_font_description (UIConfiguration::instance ().get_NormalFont ());
}

void
CueMaster::set_default_colors ()
{
	set_fill_color (HSV (UIConfiguration::instance ().color ("theme:bg")).darker (0.25).color ());
	name_text->set_color (UIConfiguration::instance ().color ("neutral:foreground"));
	stop_shape->set_outline_color (UIConfiguration::instance ().color ("neutral:midground"));
}

void
CueMaster::ui_parameter_changed (std::string const& p)
{
	if (p == "color-file") {
		set_default_colors ();
	}
}
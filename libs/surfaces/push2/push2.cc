/*
 * Copyright (C) 2016-2018 Paul Davis <paul@linuxaudiosystems.com>
 * Copyright (C) 2017-2018 Robin Gareus <robin@gareus.org>
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

#include <algorithm>
#include <bitset>

#include <stdlib.h>
#include <pthread.h>

#include "pbd/compose.h"
#include "pbd/convert.h"
#include "pbd/debug.h"
#include "pbd/failed_constructor.h"
#include "pbd/file_utils.h"
#include "pbd/search_path.h"
#include "pbd/enumwriter.h"

#include "midi++/parser.h"

#include "temporal/time.h"
#include "temporal/bbt_time.h"

#include "ardour/amp.h"
#include "ardour/async_midi_port.h"
#include "ardour/audioengine.h"
#include "ardour/debug.h"
#include "ardour/midiport_manager.h"
#include "ardour/midi_track.h"
#include "ardour/midi_port.h"
#include "ardour/session.h"
#include "ardour/tempo.h"
#include "ardour/types_convert.h"

#include "gtkmm2ext/gui_thread.h"
#include "gtkmm2ext/rgb_macros.h"

#include "gtkmm2ext/colors.h"

#include "canvas.h"
#include "cues.h"
#include "gui.h"
#include "layout.h"
#include "mix.h"
#include "push2.h"
#include "scale.h"
#include "splash.h"
#include "track_mix.h"

#include "pbd/i18n.h"

#ifdef PLATFORM_WINDOWS
#define random() rand()
#endif

using namespace ARDOUR;
using namespace PBD;
using namespace Glib;
using namespace ArdourSurface;
using namespace Gtkmm2ext;

#include "pbd/abstract_ui.cc" // instantiate template

#define ABLETON 0x2982
#define PUSH2   0x1967

static int
row_interval_semitones (const Push2::RowInterval row_interval, const bool inkey)
{
	switch (row_interval) {
	case Push2::Third:
		return 4;
	case Push2::Fourth:
		return 5;
	case Push2::Fifth:
		return 7;
	case Push2::Sequential:
		return inkey ? 12 : 8;
	}

	return 5;
}

Push2::Push2 (ARDOUR::Session& s)
	: ControlProtocol (s, std::string (X_("Ableton Push 2")))
	, AbstractUI<Push2Request> (name())
	, _handle (0)
	, _in_use (false)
	, _modifier_state (None)
	, _splash_start (0)
	, _current_layout (0)
	, _previous_layout (0)
	, _connection_state (ConnectionState (0))
	, _gui (0)
	, _mode (MusicalMode::IonianMajor)
	, _row_interval (Fourth)
	, _scale_root (0)
	, _root_octave (3)
	, _in_key (true)
	, _octave_shift (0)
	, _percussion (false)
	, _pressure_mode (AfterTouch)
	, _selection_color (LED::Green)
	, _contrast_color (LED::Green)
	, _in_range_select (false)
	, _stop_down (false)
{
	/* we're going to need this */

	libusb_init (NULL);

	build_maps ();
	build_color_map ();
	fill_color_table ();

	/* master cannot be removed, so no need to connect to going-away signal */
	_master = session->master_out ();

	/* allocate graphics layouts, even though we're not using them yet */

	_canvas = new Push2Canvas (*this, 960, 160);
	_mix_layout = new MixLayout (*this, *session, "globalmix");
	_scale_layout = new ScaleLayout (*this, *session, "scale");
	_track_mix_layout = new TrackMixLayout (*this, *session, "trackmix");
	_cue_layout = new CueLayout (*this, *session, "cues");
	_splash_layout = new SplashLayout (*this, *session, "splash");

	run_event_loop ();

	/* Ports exist for the life of this instance */

	ports_acquire ();

	/* catch arrival and departure of Push2 itself */
	ARDOUR::AudioEngine::instance()->PortRegisteredOrUnregistered.connect (port_connections, MISSING_INVALIDATOR, boost::bind (&Push2::port_registration_handler, this), this);

	/* Catch port connections and disconnections */
	ARDOUR::AudioEngine::instance()->PortConnectedOrDisconnected.connect (port_connections, MISSING_INVALIDATOR, boost::bind (&Push2::connection_handler, this, _1, _2, _3, _4, _5), this);

	/* Push 2 ports might already be there */
	port_registration_handler ();
}

Push2::~Push2 ()
{
	DEBUG_TRACE (DEBUG::Push2, "push2 control surface object being destroyed\n");

	/* do this before stopping the event loop, so that we don't get any notifications */
	port_connections.drop_connections ();

	stop_using_device ();
	device_release ();
	ports_release ();

	if (_current_layout) {
		_canvas->root()->remove (_current_layout);
		_current_layout = 0;
	}

	delete _mix_layout;
	_mix_layout = 0;
	delete _scale_layout;
	_scale_layout = 0;
	delete _splash_layout;
	_splash_layout = 0;
	delete _track_mix_layout;
	_track_mix_layout = 0;
	delete _cue_layout;
	_cue_layout = 0;

	stop_event_loop ();
}


void
Push2::run_event_loop ()
{
	DEBUG_TRACE (DEBUG::Push2, "start event loop\n");
	BaseUI::run ();
}

void
Push2::stop_event_loop ()
{
	DEBUG_TRACE (DEBUG::Push2, "stop event loop\n");
	BaseUI::quit ();
}

int
Push2::begin_using_device ()
{
	DEBUG_TRACE (DEBUG::Push2, "begin using device\n");

	/* set up periodic task used to push a frame buffer to the
	 * device (25fps). The device can handle 60fps, but we don't
	 * need that frame rate.
	 */

	Glib::RefPtr<Glib::TimeoutSource> vblank_timeout = Glib::TimeoutSource::create (40); // milliseconds
	_vblank_connection = vblank_timeout->connect (sigc::mem_fun (*this, &Push2::vblank));
	vblank_timeout->attach (main_loop()->get_context());

	connect_session_signals ();

	init_buttons (true);
	init_touch_strip ();
	reset_pad_colors ();
	splash ();

	/* catch current selection, if any so that we can wire up the pads if appropriate */
	stripable_selection_changed ();

	request_pressure_mode ();

	_in_use = true;

	return 0;
}

int
Push2::stop_using_device ()
{
	DEBUG_TRACE (DEBUG::Push2, "stop using device\n");

	if (!_in_use) {
		DEBUG_TRACE (DEBUG::Push2, "nothing to do, device not in use\n");
		return 0;
	}

	init_buttons (false);
	strip_buttons_off ();

	for (auto & pad : _xy_pad_map) {
		pad->set_color (LED::Black);
		pad->set_state (LED::NoTransition);
		write (pad->state_msg());
	}

	_vblank_connection.disconnect ();
	session_connections.drop_connections ();

	_in_use = false;
	return 0;
}

int
Push2::ports_acquire ()
{
	DEBUG_TRACE (DEBUG::Push2, "acquiring ports\n");

	/* setup ports */

	_async_in  = AudioEngine::instance()->register_input_port (DataType::MIDI, X_("Push 2 in"), true);
	_async_out = AudioEngine::instance()->register_output_port (DataType::MIDI, X_("Push 2 out"), true);

	if (_async_in == 0 || _async_out == 0) {
		DEBUG_TRACE (DEBUG::Push2, "cannot register ports\n");
		return -1;
	}

	/* We do not add our ports to the input/output bundles because we don't
	 * want users wiring them by hand. They could use JACK tools if they
	 * really insist on that (and use JACK)
	 */

	_input_port = boost::dynamic_pointer_cast<AsyncMIDIPort>(_async_in).get();
	_output_port = boost::dynamic_pointer_cast<AsyncMIDIPort>(_async_out).get();

	/* Create a shadow port where, depending on the state of the surface,
	 * we will make pad note on/off events appear. The surface code will
	 * automatically this port to the first selected MIDI track.
	 */

	boost::dynamic_pointer_cast<AsyncMIDIPort>(_async_in)->add_shadow_port (string_compose (_("%1 Pads"), X_("Push 2")), boost::bind (&Push2::pad_filter, this, _1, _2));
	boost::shared_ptr<MidiPort> shadow_port = boost::dynamic_pointer_cast<AsyncMIDIPort>(_async_in)->shadow_port();

	if (shadow_port) {

		_output_bundle.reset (new ARDOUR::Bundle (_("Push 2 Pads"), false));

		_output_bundle->add_channel (
			shadow_port->name(),
			ARDOUR::DataType::MIDI,
			session->engine().make_port_name_non_relative (shadow_port->name())
			);
	}

	session->BundleAddedOrRemoved ();

	connect_to_parser ();

	/* Connect input port to event loop */

	AsyncMIDIPort* asp;

	asp = dynamic_cast<AsyncMIDIPort*> (_input_port);
	asp->xthread().set_receive_handler (sigc::bind (sigc::mem_fun (this, &Push2::midi_input_handler), _input_port));
	asp->xthread().attach (main_loop()->get_context());

	return 0;
}

void
Push2::ports_release ()
{
	DEBUG_TRACE (DEBUG::Push2, "releasing ports\n");

	/* wait for button data to be flushed */
	AsyncMIDIPort* asp;
	asp = dynamic_cast<AsyncMIDIPort*> (_output_port);
	asp->drain (10000, 500000);

	{
		Glib::Threads::Mutex::Lock em (AudioEngine::instance()->process_lock());
		AudioEngine::instance()->unregister_port (_async_in);
		AudioEngine::instance()->unregister_port (_async_out);
	}

	_async_in.reset ((ARDOUR::Port*) 0);
	_async_out.reset ((ARDOUR::Port*) 0);
	_input_port = 0;
	_output_port = 0;
}

int
Push2::device_acquire ()
{
	int err;

	DEBUG_TRACE (DEBUG::Push2, "acquiring device\n");

	if (_handle) {
		DEBUG_TRACE (DEBUG::Push2, "open() called with handle already set\n");
		/* already open */
		return 0;
	}

	if ((_handle = libusb_open_device_with_vid_pid (NULL, ABLETON, PUSH2)) == 0) {
		DEBUG_TRACE (DEBUG::Push2, "failed to open USB handle\n");
		return -1;
	}

	if ((err = libusb_claim_interface (_handle, 0x00))) {
		DEBUG_TRACE (DEBUG::Push2, "failed to claim USB device\n");
		libusb_close (_handle);
		_handle = 0;
		return -1;
	}

	return 0;
}

void
Push2::device_release ()
{
	DEBUG_TRACE (DEBUG::Push2, "releasing device\n");
	if (_handle) {
		libusb_release_interface (_handle, 0x00);
		libusb_close (_handle);
		_handle = 0;
	}
}

list<boost::shared_ptr<ARDOUR::Bundle> >
Push2::bundles ()
{
	list<boost::shared_ptr<ARDOUR::Bundle> > b;

	if (_output_bundle) {
		b.push_back (_output_bundle);
	}

	return b;
}

void
Push2::strip_buttons_off ()
{
	ButtonID strip_buttons[] = { Upper1, Upper2, Upper3, Upper4, Upper5, Upper6, Upper7, Upper8,
	                             Lower1, Lower2, Lower3, Lower4, Lower5, Lower6, Lower7, Lower8, };

	for (size_t n = 0; n < sizeof (strip_buttons) / sizeof (strip_buttons[0]); ++n) {
		boost::shared_ptr<Button> b = _id_button_map[strip_buttons[n]];

		b->set_color (LED::Black);
		b->set_state (LED::OneShot24th);
		write (b->state_msg());
	}
}


void
Push2::init_buttons (bool startup)
{
	/* This is a list of buttons that we want lit because they do something
	   in ardour related (loosely, sometimes) to their illuminated label.
	*/


	if (startup) {

		ButtonID buttons[] = { Mute, Solo, Master, Up, Right, Left, Down, Note, Session, Mix, AddTrack, Delete, Undo,
			Metronome, Shift, Select, Play, RecordEnable, Automate, Repeat, Note, Session,
			Quantize, Duplicate, Browse, PageRight, PageLeft, OctaveUp, OctaveDown, Layout, Scale,
			Stop
		};


		for (size_t n = 0; n < sizeof (buttons) / sizeof (buttons[0]); ++n) {
			boost::shared_ptr<Button> b = _id_button_map[buttons[n]];

			b->set_color (LED::White);
			b->set_state (LED::NoTransition);
			write (b->state_msg());
		}

		/* all other buttons are off (black) */

		ButtonID off_buttons[] = { TapTempo, Setup, User, Convert, New, FixedLength, Clip,
		                           Fwd32ndT, Fwd32nd, Fwd16thT, Fwd16th, Fwd8thT, Fwd8th, Fwd4trT, Fwd4tr,
		                           Accent, Note };

		for (size_t n = 0; n < sizeof (off_buttons) / sizeof (off_buttons[0]); ++n) {
			boost::shared_ptr<Button> b = _id_button_map[off_buttons[n]];

			b->set_color (LED::Black);
			b->set_state (LED::OneShot24th);
			write (b->state_msg());
		}

	} else {

		if (_current_layout) {
			_current_layout->hide ();
		}

		for (auto & b : _id_button_map) {
			b.second->set_color (LED::Black);
			b.second->set_state (LED::NoTransition);
			write (b.second->state_msg());
		}
	}
}

bool
Push2::probe ()
{
	return true;
}

void*
Push2::request_factory (uint32_t num_requests)
{
	/* AbstractUI<T>::request_buffer_factory() is a template method only
	   instantiated in this source module. To provide something visible for
	   use in the interface/descriptor, we have this static method that is
	   template-free.
	*/
	return request_buffer_factory (num_requests);
}

void
Push2::do_request (Push2Request * req)
{
	if (req->type == CallSlot) {

		call_slot (MISSING_INVALIDATOR, req->the_slot);

	} else if (req->type == Quit) {

		stop_using_device ();
	}
}

void
Push2::splash ()
{
	set_current_layout (_splash_layout);
	_splash_start = get_microseconds ();
}

bool
Push2::vblank ()
{
	if (_splash_start) {

		/* display splash for 2 seconds */

		if (get_microseconds() - _splash_start > 2000000) {
			_splash_start = 0;
			DEBUG_TRACE (DEBUG::Push2, "splash interval ended, switch to mix layout\n");
			set_current_layout (_mix_layout);
		}
	}

	if (_current_layout) {
		_current_layout->update_meters ();
		_current_layout->update_clocks ();
	}

	_canvas->vblank();

	return true;
}

int
Push2::set_active (bool yn)
{
	DEBUG_TRACE (DEBUG::Push2, string_compose("Push2Protocol::set_active init with yn: '%1'\n", yn));

	if (yn == active()) {
		return 0;
	}

	if (yn) {

		if (device_acquire ()) {
			return -1;
		}

		if ((_connection_state & (InputConnected|OutputConnected)) == (InputConnected|OutputConnected)) {
			begin_using_device ();
		} else {
			/* begin_using_device () will get called once we're connected */
		}

	} else {
		/* Control Protocol Manager never calls us with false, but
		 * insteads destroys us.
		 */
	}

	ControlProtocol::set_active (yn);

	DEBUG_TRACE (DEBUG::Push2, string_compose("Push2Protocol::set_active done with yn: '%1'\n", yn));

	return 0;
}

void
Push2::init_touch_strip ()
{
	MidiByteArray msg (9, 0xf0, 0x00, 0x21, 0x1d, 0x01, 0x01, 0x17, 0x00, 0xf7);
	/* flags are the final byte (ignore end-of-sysex */

	/* show bar, not point
	   autoreturn to center
	   bar starts at center
	*/
	msg[7] = (1<<4) | (1<<5) | (1<<6);
	write (msg);
}

void
Push2::write (const MidiByteArray& data)
{
	/* immediate delivery */
	_output_port->write (&data[0], data.size(), 0);
}

bool
Push2::midi_input_handler (IOCondition ioc, MIDI::Port* port)
{
	if (ioc & ~IO_IN) {
		DEBUG_TRACE (DEBUG::Push2, "MIDI port closed\n");
		return false;
	}

	if (ioc & IO_IN) {

		DEBUG_TRACE (DEBUG::Push2, string_compose ("something happened on  %1\n", port->name()));

		AsyncMIDIPort* asp = dynamic_cast<AsyncMIDIPort*>(port);
		if (asp) {
			asp->clear ();
		}

		DEBUG_TRACE (DEBUG::Push2, string_compose ("data available on %1\n", port->name()));
		if (_in_use) {
			samplepos_t now = AudioEngine::instance()->sample_time();
			port->parse (now);
		}
	}

	return true;
}

void
Push2::connect_to_parser ()
{
	DEBUG_TRACE (DEBUG::Push2, string_compose ("Connecting to signals on port %2\n", _input_port->name()));

	MIDI::Parser* p = _input_port->parser();

	/* Incoming sysex */
	p->sysex.connect_same_thread (*this, boost::bind (&Push2::handle_midi_sysex, this, _1, _2, _3));
	/* V-Pot messages are Controller */
	p->controller.connect_same_thread (*this, boost::bind (&Push2::handle_midi_controller_message, this, _1, _2));
	/* Button messages are NoteOn */
	p->note_on.connect_same_thread (*this, boost::bind (&Push2::handle_midi_note_on_message, this, _1, _2));
	/* Button messages are NoteOn but libmidi++ sends note-on w/velocity = 0 as note-off so catch them too */
	p->note_off.connect_same_thread (*this, boost::bind (&Push2::handle_midi_note_on_message, this, _1, _2));
	/* Fader messages are Pitchbend */
	p->channel_pitchbend[0].connect_same_thread (*this, boost::bind (&Push2::handle_midi_pitchbend_message, this, _1, _2));
}

void
Push2::handle_midi_sysex (MIDI::Parser&, MIDI::byte* raw_bytes, size_t sz)
{
	DEBUG_TRACE (DEBUG::Push2, string_compose ("Sysex, %1 bytes\n", sz));

	if (sz < 8) {
		return;
	}

	MidiByteArray msg (sz, raw_bytes);
	MidiByteArray push2_sysex_header (6, 0xF0, 0x00, 0x21, 0x1D, 0x01, 0x01);

	if (!push2_sysex_header.compare_n (msg, 6)) {
		return;
	}

	switch (msg[6]) {
	case 0x1f: /* pressure mode */
		if (msg[7] == 0x0) {
			_pressure_mode = AfterTouch;
			PressureModeChange (AfterTouch);
		} else {
			_pressure_mode = PolyPressure;
			PressureModeChange (PolyPressure);
		}
		break;
	}
}

void
Push2::handle_midi_controller_message (MIDI::Parser&, MIDI::EventTwoBytes* ev)
{
	DEBUG_TRACE (DEBUG::Push2, string_compose ("CC %1 (value %2)\n", (int) ev->controller_number, (int) ev->value));

	CCButtonMap::iterator b = _cc_button_map.find (ev->controller_number);

	if (ev->value) {
		/* any press cancels any pending long press timeouts */
		for (std::set<ButtonID>::iterator x = _buttons_down.begin(); x != _buttons_down.end(); ++x) {
			boost::shared_ptr<Button> bb = _id_button_map[*x];
			bb->timeout_connection.disconnect ();
		}
	}

	if (b != _cc_button_map.end()) {

		boost::shared_ptr<Button> button = b->second;

		if (ev->value) {
			_buttons_down.insert (button->id);
			start_press_timeout (button, button->id);
		} else {
			_buttons_down.erase (button->id);
			button->timeout_connection.disconnect ();
		}


		std::set<ButtonID>::iterator c = _consumed.find (button->id);

		if (c == _consumed.end()) {
			if (ev->value == 0) {
				(this->*button->release_method)();
			} else {
				(this->*button->press_method)();
			}
		} else {
			DEBUG_TRACE (DEBUG::Push2, "button was consumed, ignored\n");
			_consumed.erase (c);
		}

	} else {

		/* encoder/vpot */

		int delta = ev->value;

		if (delta > 63) {
			delta = -(128 - delta);
		}

		switch (ev->controller_number) {
		case 71:
			_current_layout->strip_vpot (0, delta);
			break;
		case 72:
			_current_layout->strip_vpot (1, delta);
			break;
		case 73:
			_current_layout->strip_vpot (2, delta);
			break;
		case 74:
			_current_layout->strip_vpot (3, delta);
			break;
		case 75:
			_current_layout->strip_vpot (4, delta);
			break;
		case 76:
			_current_layout->strip_vpot (5, delta);
			break;
		case 77:
			_current_layout->strip_vpot (6, delta);
			break;
		case 78:
			_current_layout->strip_vpot (7, delta);
			break;

			/* left side pair */
		case 14:
			other_vpot (8, delta);
			break;
		case 15:
			other_vpot (1, delta);
			break;

			/* right side */
		case 79:
			other_vpot (2, delta);
			break;
		}
	}
}

void
Push2::handle_midi_note_on_message (MIDI::Parser& parser, MIDI::EventTwoBytes* ev)
{
	// DEBUG_TRACE (DEBUG::Push2, string_compose ("Note On %1 (velocity %2)\n", (int) ev->note_number, (int) ev->velocity));

	if (ev->velocity == 0) {
		handle_midi_note_off_message (parser, ev);
		return;
	}

	switch (ev->note_number) {
	case 0:
		_current_layout->strip_vpot_touch (0, ev->velocity > 64);
		break;
	case 1:
		_current_layout->strip_vpot_touch (1, ev->velocity > 64);
		break;
	case 2:
		_current_layout->strip_vpot_touch (2, ev->velocity > 64);
		break;
	case 3:
		_current_layout->strip_vpot_touch (3, ev->velocity > 64);
		break;
	case 4:
		_current_layout->strip_vpot_touch (4, ev->velocity > 64);
		break;
	case 5:
		_current_layout->strip_vpot_touch (5, ev->velocity > 64);
		break;
	case 6:
		_current_layout->strip_vpot_touch (6, ev->velocity > 64);
		break;
	case 7:
		_current_layout->strip_vpot_touch (7, ev->velocity > 64);
		break;

		/* left side */
	case 10:
		other_vpot_touch (0, ev->velocity > 64);
		break;
	case 9:
		other_vpot_touch (1, ev->velocity > 64);
		break;

		/* right side */
	case 8:
		other_vpot_touch (3, ev->velocity > 64);
		break;

		/* touch strip */
	case 12:
		if (ev->velocity < 64) {
			transport_stop ();
		}
		break;
	}

	if (ev->note_number < 11) {
		return;
	}

	/* Pad illuminations */

	PadMap::const_iterator pm = _nn_pad_map.find (ev->note_number);

	if (pm == _nn_pad_map.end()) {
		return;
	}

	boost::shared_ptr<const Pad> pad_pressed = pm->second;

	if (_current_layout == _cue_layout) {
		_current_layout->pad_press (pad_pressed->x, pad_pressed->y);
		return;
	}

	pair<FNPadMap::iterator,FNPadMap::iterator> pads_with_note = _fn_pad_map.equal_range (pad_pressed->filtered);

	if (pads_with_note.first == _fn_pad_map.end()) {
		return;
	}

	for (FNPadMap::iterator pi = pads_with_note.first; pi != pads_with_note.second; ++pi) {
		boost::shared_ptr<Pad> pad = pi->second;

		if (pad->do_when_pressed == Pad::FlashOn) {
			pad->set_color (_contrast_color);
			pad->set_state (LED::NoTransition);
		} else if (pad->do_when_pressed == Pad::FlashOff) {
			pad->set_color (LED::Black);
			pad->set_state (LED::NoTransition);
		}
		write (pad->state_msg());
	}
}

void
Push2::handle_midi_note_off_message (MIDI::Parser&, MIDI::EventTwoBytes* ev)
{
	// DEBUG_TRACE (DEBUG::Push2, string_compose ("Note Off %1 (velocity %2)\n", (int) ev->note_number, (int) ev->velocity));

	if (ev->note_number < 11) {
		/* theoretically related to encoder touch start/end, but
		 * actually they send note on with two different velocity
		 * values (127 & 64).
		 */
		return;
	}

	/* Pad illuminations */

	PadMap::const_iterator pm = _nn_pad_map.find (ev->note_number);

	if (pm == _nn_pad_map.end()) {
		return;
	}

	boost::shared_ptr<const Pad> const pad_pressed = pm->second;

	if (_current_layout == _cue_layout) {
		_current_layout->pad_release (pad_pressed->x, pad_pressed->y);
		return;
	}
	pair<FNPadMap::iterator,FNPadMap::iterator> pads_with_note = _fn_pad_map.equal_range (pad_pressed->filtered);

	if (pads_with_note.first == _fn_pad_map.end()) {
		return;
	}

	for (FNPadMap::iterator pi = pads_with_note.first; pi != pads_with_note.second; ++pi) {
		boost::shared_ptr<Pad> pad = pi->second;

		pad->set_color (pad->perma_color);
		pad->set_state (LED::NoTransition);
		write (pad->state_msg());
	}
}

void
Push2::handle_midi_pitchbend_message (MIDI::Parser&, MIDI::pitchbend_t pb)
{
}

void
Push2::thread_init ()
{
	pthread_set_name (event_loop_name().c_str());

	PBD::notify_event_loops_about_thread_creation (pthread_self(), event_loop_name(), 2048);
	ARDOUR::SessionEvent::create_per_thread_pool (event_loop_name(), 128);

	set_thread_priority ();
}

void
Push2::connect_session_signals()
{
	// receive routes added
	//session->RouteAdded.connect(session_connections, MISSING_INVALIDATOR, boost::bind (&MackieControlProtocol::notify_routes_added, this, _1), this);
	// receive VCAs added
	//session->vca_manager().VCAAdded.connect(session_connections, MISSING_INVALIDATOR, boost::bind (&Push2::notify_vca_added, this, _1), this);

	// receive record state toggled
	session->RecordStateChanged.connect(session_connections, MISSING_INVALIDATOR, boost::bind (&Push2::notify_record_state_changed, this), this);
	// receive transport state changed
	session->TransportStateChange.connect(session_connections, MISSING_INVALIDATOR, boost::bind (&Push2::notify_transport_state_changed, this), this);
	session->TransportLooped.connect (session_connections, MISSING_INVALIDATOR, boost::bind (&Push2::notify_loop_state_changed, this), this);
	// receive punch-in and punch-out
	Config->ParameterChanged.connect(session_connections, MISSING_INVALIDATOR, boost::bind (&Push2::notify_parameter_changed, this, _1), this);
	session->config.ParameterChanged.connect (session_connections, MISSING_INVALIDATOR, boost::bind (&Push2::notify_parameter_changed, this, _1), this);
	// receive rude solo changed
	session->SoloActive.connect(session_connections, MISSING_INVALIDATOR, boost::bind (&Push2::notify_solo_active_changed, this, _1), this);
}

void
Push2::notify_record_state_changed ()
{
	IDButtonMap::iterator b = _id_button_map.find (RecordEnable);

	if (b == _id_button_map.end()) {
		return;
	}

	switch (session->record_status ()) {
	case Session::Disabled:
		b->second->set_color (LED::White);
		b->second->set_state (LED::NoTransition);
		break;
	case Session::Enabled:
		b->second->set_color (LED::Red);
		b->second->set_state (LED::Blinking4th);
		break;
	case Session::Recording:
		b->second->set_color (LED::Red);
		b->second->set_state (LED::OneShot24th);
		break;
	}

	write (b->second->state_msg());
}

void
Push2::notify_transport_state_changed ()
{
	boost::shared_ptr<Button> b = _id_button_map[Play];

	if (session->transport_rolling()) {
		b->set_state (LED::OneShot24th);
		b->set_color (LED::Green);
	} else {

		/* disable any blink on FixedLength from pending edit range op */
		boost::shared_ptr<Button> fl = _id_button_map[FixedLength];

		fl->set_color (LED::Black);
		fl->set_state (LED::NoTransition);
		write (fl->state_msg());

		b->set_color (LED::White);
		b->set_state (LED::NoTransition);
	}

	write (b->state_msg());
}

void
Push2::notify_loop_state_changed ()
{
}

void
Push2::notify_parameter_changed (std::string param)
{
	IDButtonMap::iterator b;

	if (param == "clicking") {
		if ((b = _id_button_map.find (Metronome)) == _id_button_map.end()) {
			return;
		}
		if (Config->get_clicking()) {
			b->second->set_state (LED::Blinking4th);
			b->second->set_color (LED::White);
		} else {
			b->second->set_color (LED::White);
			b->second->set_state (LED::NoTransition);
		}
		write (b->second->state_msg ());
	}
}

void
Push2::notify_solo_active_changed (bool yn)
{
	IDButtonMap::iterator b = _id_button_map.find (Solo);

	if (b == _id_button_map.end()) {
		return;
	}

	if (yn) {
		b->second->set_state (LED::Blinking4th);
		b->second->set_color (LED::Red);
	} else {
		b->second->set_state (LED::NoTransition);
		b->second->set_color (LED::White);
	}

	write (b->second->state_msg());
}

XMLNode&
Push2::get_state() const
{
	XMLNode& node (ControlProtocol::get_state());
	XMLNode* child;

	child = new XMLNode (X_("Input"));
	child->add_child_nocopy (_async_in->get_state());
	node.add_child_nocopy (*child);
	child = new XMLNode (X_("Output"));
	child->add_child_nocopy (_async_out->get_state());
	node.add_child_nocopy (*child);

	node.set_property (X_("root"), _scale_root);
	node.set_property (X_("root-octave"), _root_octave);
	node.set_property (X_("in-key"), _in_key);
	node.set_property (X_("mode"), _mode);

	return node;
}

int
Push2::set_state (const XMLNode & node, int version)
{
	DEBUG_TRACE (DEBUG::Push2, string_compose ("Push2::set_state: active %1\n", active()));

	int retval = 0;

	if (ControlProtocol::set_state (node, version)) {
		return -1;
	}

	XMLNode* child;

	if ((child = node.child (X_("Input"))) != 0) {
		XMLNode* portnode = child->child (Port::state_node_name.c_str());
		if (portnode) {
			portnode->remove_property ("name");
			_async_in->set_state (*portnode, version);
		}
	}

	if ((child = node.child (X_("Output"))) != 0) {
		XMLNode* portnode = child->child (Port::state_node_name.c_str());
		if (portnode) {
			portnode->remove_property ("name");
			_async_out->set_state (*portnode, version);
		}
	}

	node.get_property (X_("root"), _scale_root);
	node.get_property (X_("root-octave"), _root_octave);
	node.get_property (X_("in-key"), _in_key);
	node.get_property (X_("mode"), _mode);

	return retval;
}

void
Push2::other_vpot (int n, int delta)
{
	boost::shared_ptr<Amp> click_gain;
	switch (n) {
	case 0:
		/* tempo control */
		break;
	case 1:
		/* metronome gain control */
		click_gain = session->click_gain();
		if (click_gain) {
			boost::shared_ptr<AutomationControl> ac = click_gain->gain_control();
			if (ac) {
				ac->set_value (ac->interface_to_internal (
					               min (ac->upper(), max (ac->lower(), ac->internal_to_interface (ac->get_value()) + (delta/256.0)))),
				               PBD::Controllable::UseGroup);
			}
		}
		break;
	case 2:
		/* master gain control */
		if (_master) {
			boost::shared_ptr<AutomationControl> ac = _master->gain_control();
			if (ac) {
				ac->set_value (ac->interface_to_internal (
					               min (ac->upper(), max (ac->lower(), ac->internal_to_interface (ac->get_value()) + (delta/256.0)))),
				               PBD::Controllable::UseGroup);
			}
		}
		break;
	}
}

void
Push2::other_vpot_touch (int n, bool touching)
{
	switch (n) {
	case 0:
		break;
	case 1:
		break;
	case 2:
		if (_master) {
			boost::shared_ptr<AutomationControl> ac = _master->gain_control();
			if (ac) {
				const timepos_t now (session->audible_sample());
				if (touching) {
					ac->start_touch (now);
				} else {
					ac->stop_touch (now);
				}
			}
		}
	}
}

void
Push2::start_shift ()
{
	_modifier_state = ModifierState (_modifier_state | ModShift);
	boost::shared_ptr<Button> b = _id_button_map[Shift];
	b->set_color (LED::White);
	b->set_state (LED::Blinking16th);
	write (b->state_msg());
}

void
Push2::end_shift ()
{
	if (_modifier_state & ModShift) {
		_modifier_state = ModifierState (_modifier_state & ~(ModShift));
		boost::shared_ptr<Button> b = _id_button_map[Shift];
		b->timeout_connection.disconnect ();
		b->set_color (LED::White);
		b->set_state (LED::OneShot24th);
		write (b->state_msg());
	}
}

bool
Push2::pad_filter (MidiBuffer& in, MidiBuffer& out) const
{
	/* This filter is called asynchronously from a realtime process
	   context. It must use atomics to check state, and must not block.
	*/

	if (_current_layout == _cue_layout) {
		return false;
	}

	bool matched = false;

	for (MidiBuffer::iterator ev = in.begin(); ev != in.end(); ++ev) {
		if ((*ev).is_note_on() || (*ev).is_note_off()) {

			/* encoder touch start/touch end use note
			 * 0-10. touchstrip uses note 12
			 */

			if ((*ev).note() > 10 && (*ev).note() != 12) {

				const int n = (*ev).note ();
				PadMap::const_iterator nni = _nn_pad_map.find (n);

				if (nni != _nn_pad_map.end()) {
					boost::shared_ptr<const Pad> pad = nni->second;
					/* shift for output to the shadow port */
					if (pad->filtered >= 0) {
						(*ev).set_note (pad->filtered + (_octave_shift * 12));
						out.push_back (*ev);
						/* shift back so that the pads light correctly  */
						(*ev).set_note (n);
					} else {
						/* no mapping, don't send event */
					}
				} else {
					out.push_back (*ev);
				}

				matched = true;
			}
		} else if ((*ev).is_pitch_bender() || (*ev).is_poly_pressure() || (*ev).is_channel_pressure()) {
			out.push_back (*ev);
		}
	}

	return matched;
}

void
Push2::port_registration_handler ()
{
	if (!_async_in || !_async_out) {
		/* ports not registered yet */
		return;
	}

	if (_async_in->connected() && _async_out->connected()) {
		/* don't waste cycles here */
		return;
	}

#ifdef __APPLE__
	/* the origin of the numeric magic identifiers is known only to Ableton
	   and may change in time. This is part of how CoreMIDI works.
	*/
	std::string input_port_name = X_("system:midi_capture_1319078870");
	std::string output_port_name = X_("system:midi_playback_3409210341");
#else
	std::string input_port_name = X_("Ableton Push 2 MIDI 1 in");
	std::string output_port_name = X_("Ableton Push 2 MIDI 1 out");
#endif
	std::vector<std::string> in;
	std::vector<std::string> out;

	AudioEngine::instance()->get_ports (string_compose (".*%1", input_port_name), DataType::MIDI, PortFlags (IsPhysical|IsOutput), in);
	AudioEngine::instance()->get_ports (string_compose (".*%1", output_port_name), DataType::MIDI, PortFlags (IsPhysical|IsInput), out);

	if (!in.empty() && !out.empty()) {
		if (!_async_in->connected()) {
			AudioEngine::instance()->connect (_async_in->name(), in.front());
		}
		if (!_async_out->connected()) {
			AudioEngine::instance()->connect (_async_out->name(), out.front());
		}
	}
}

bool
Push2::connection_handler (boost::weak_ptr<ARDOUR::Port>, std::string name1, boost::weak_ptr<ARDOUR::Port>, std::string name2, bool yn)
{
	DEBUG_TRACE (DEBUG::FaderPort, "FaderPort::connection_handler start\n");
	if (!_input_port || !_output_port) {
		return false;
	}

	std::string ni = ARDOUR::AudioEngine::instance()->make_port_name_non_relative (boost::shared_ptr<ARDOUR::Port>(_async_in)->name());
	std::string no = ARDOUR::AudioEngine::instance()->make_port_name_non_relative (boost::shared_ptr<ARDOUR::Port>(_async_out)->name());

	if (ni == name1 || ni == name2) {
		if (yn) {
			_connection_state |= InputConnected;
		} else {
			_connection_state &= ~InputConnected;
		}
	} else if (no == name1 || no == name2) {
		if (yn) {
			_connection_state |= OutputConnected;
		} else {
			_connection_state &= ~OutputConnected;
		}
	} else {
		DEBUG_TRACE (DEBUG::Push2, string_compose ("Connections between %1 and %2 changed, but I ignored it\n", name1, name2));
		/* not our ports */
		return false;
	}

	DEBUG_TRACE (DEBUG::Push2, string_compose ("our ports changed connection state: %1 -> %2 connected ? %3\n",
	                                           name1, name2, yn));

	if ((_connection_state & (InputConnected|OutputConnected)) == (InputConnected|OutputConnected)) {

		/* XXX this is a horrible hack. Without a short sleep here,
		   something prevents the device wakeup messages from being
		   sent and/or the responses from being received.
		*/

		g_usleep (100000);
                DEBUG_TRACE (DEBUG::Push2, "device now connected for both input and output\n");

                /* may not have the device open if it was just plugged
                   in. Really need USB device detection rather than MIDI port
                   detection for this to work well.
                */

                device_acquire ();
                begin_using_device ();

	} else {
		DEBUG_TRACE (DEBUG::FaderPort, "Device disconnected (input or output or both) or not yet fully connected\n");
		stop_using_device ();
	}

	ConnectionChange (); /* emit signal for our GUI */

	DEBUG_TRACE (DEBUG::FaderPort, "FaderPort::connection_handler  end\n");

	return true; /* connection status changed */
}

boost::shared_ptr<Port>
Push2::output_port()
{
	return _async_out;
}

boost::shared_ptr<Port>
Push2::input_port()
{
	return _async_in;
}

int
Push2::pad_note (int row, int col) const
{
	PadMap::const_iterator nni = _nn_pad_map.find (36+(row*8)+col);

	if (nni != _nn_pad_map.end()) {
		return nni->second->filtered;
	}

	return 0;
}

void
Push2::update_selection_color ()
{
	boost::shared_ptr<MidiTrack> current_midi_track = _current_pad_target.lock();

	if (!current_midi_track) {
		return;
	}

	_selection_color = get_color_index (current_midi_track->presentation_info().color());
	_contrast_color = get_color_index (Gtkmm2ext::HSV (current_midi_track->presentation_info().color()).opposite().color());

	reset_pad_colors ();
}

void
Push2::reset_pad_colors ()
{
	set_pad_scale (_scale_root, _root_octave, _mode, _note_grid_origin, _row_interval, _in_key);
}

void
Push2::set_pad_note_kind (Pad& pad, const PadNoteKind kind)
{
	switch (kind) {
	case RootNote:
		pad.set_color (_selection_color);
		pad.perma_color     = _selection_color;
		pad.do_when_pressed = Pad::FlashOff;
		break;
	case InScaleNote:
		pad.set_color (LED::White);
		pad.perma_color     = LED::White;
		pad.do_when_pressed = Pad::FlashOff;
		break;
	case OutOfScaleNote:
		pad.set_color (LED::Black);
		pad.do_when_pressed = Pad::FlashOn;
		break;
	}

	pad.set_state (LED::NoTransition);
}

/** Return a bitset of notes in a musical mode.
 *
 * The returned bitset has a bit for every possible MIDI note number, which is
 * set if the note is in the mode in any octave.
 */
static std::bitset<128>
mode_notes_bitset (const int               scale_root,
                   const int               octave,
                   const MusicalMode::Type mode)
{
	std::bitset<128> notes_bitset;

	const std::vector<float> mode_steps = MusicalMode (mode).steps;
	int                      root       = scale_root - 12;

	// Repeatedly loop through the intervals in an octave
	for (std::vector<float>::const_iterator i = mode_steps.begin ();;) {
		if (i == mode_steps.end ()) {
			// Reached the end of the scale, continue with the next octave
			root += 12;
			if (root > 127) {
				break;
			}

			notes_bitset.set (root);
			i = mode_steps.begin ();

		} else {
			const int note = (int)floor (root + (2.0 * (*i)));
			if (note > 127) {
				break;
			}

			if (note > 0) {
				notes_bitset.set (note);
			}

			++i;
		}
	}

	return notes_bitset;
}

/** Return a sorted vector of all notes in a musical mode.
 *
 * The returned vector has every possible MIDI note number (0 through 127
 * inclusive) that is in the mode in any octave.
 */
static std::vector<int>
mode_notes_vector (const int               scale_root,
                   const int               octave,
                   const MusicalMode::Type mode)
{
	std::vector<int> notes_vector;

	const std::vector<float> mode_steps = MusicalMode (mode).steps;
	int                      root       = scale_root - 12;

	// Repeatedly loop through the intervals in an octave
	for (std::vector<float>::const_iterator i = mode_steps.begin ();;) {
		if (i == mode_steps.end ()) {
			// Reached the end of the scale, continue with the next octave
			root += 12;
			if (root > 127) {
				break;
			}

			notes_vector.push_back (root);
			i = mode_steps.begin ();

		} else {
			const int note = (int)floor (root + (2.0 * (*i)));
			if (note > 127) {
				break;
			}

			if (note > 0) {
				notes_vector.push_back (note);
			}

			++i;
		}
	}

	return notes_vector;
}

void
Push2::set_pad_scale_in_key (const int               scale_root,
                             const int               octave,
                             const MusicalMode::Type mode,
                             const NoteGridOrigin    origin,
                             const int               ideal_vertical_semitones)
{
	const std::vector<int> notes = mode_notes_vector (scale_root, octave, mode);

	const int ideal_first_note = origin == Fixed ? 36 : scale_root + (12 * octave);

	for (int row = 0; row < 8; ++row) {
		// The ideal leftmost note in a row is based only on the "tuning"
		const int ideal_leftmost_note =
		  ideal_first_note + (ideal_vertical_semitones * row);

		// If that's in the scale, use it, otherwise use the closest higher note
		std::vector<int>::const_iterator n =
		  std::lower_bound (notes.begin (), notes.end (), ideal_leftmost_note);

		// Set up the the following columns in the row using the scale
		for (int col = 0; col < 8 && n != notes.end (); ++col) {
			const int                     note  = *n++;
			const int                     index = 36 + (row * 8) + col;
			const boost::shared_ptr<Pad>& pad   = _nn_pad_map[index];

			pad->filtered = note; // Generated note number

			_fn_pad_map.insert (std::make_pair (note, pad));

			if ((note % 12) == scale_root) {
				set_pad_note_kind (*pad, RootNote);
			} else {
				set_pad_note_kind (*pad, InScaleNote);
			}
		}
	}
}

void
Push2::restore_pad_scale ()
{
	set_pad_scale (_scale_root, _root_octave, _mode, _note_grid_origin, _row_interval, _in_key);
}

void
Push2::set_pad_scale_chromatic (const int               scale_root,
                                const int               octave,
                                const MusicalMode::Type mode,
                                const NoteGridOrigin    origin,
                                const int               vertical_semitones)
{
	const std::bitset<128> notes = mode_notes_bitset (scale_root, octave, mode);

	const int first_note = origin == Fixed ? 36 : scale_root + (12 * octave);

	for (int row = 0; row < 8; ++row) {
		// The leftmost note in a row is just based only on the "tuning"
		const int leftmost_note = first_note + (vertical_semitones * row);

		// Set up the the following columns in the row using the scale
		for (int col = 0; col < 8; ++col) {
			const int                     note  = leftmost_note + col;
			const int                     index = 36 + (row * 8) + col;
			const boost::shared_ptr<Pad>& pad   = _nn_pad_map[index];

			pad->filtered = note; // Generated note number

			_fn_pad_map.insert (std::make_pair (note, pad));

			if (!notes.test (note)) {
				set_pad_note_kind (*pad, OutOfScaleNote);
			} else if ((note % 12) == scale_root) {
				set_pad_note_kind (*pad, RootNote);
			} else {
				set_pad_note_kind (*pad, InScaleNote);
			}
		}
	}
}

void
Push2::set_pad_scale (const int               scale_root,
                      const int               octave,
                      const MusicalMode::Type mode,
                      const NoteGridOrigin    origin,
                      const RowInterval       row_interval,
                      const bool              inkey)
{
	// Clear the pad map and reset all pad state (in memory, not on the device yet)

	_fn_pad_map.clear ();

	for (auto & p : _nn_pad_map) {
		p.second->set_color (LED::Black);
		p.second->set_state (LED::NoTransition);
		p.second->perma_color = LED::Black;
		p.second->filtered        = -1;
		p.second->do_when_pressed = Pad::FlashOn;
	}

	// Call the appropriate method to set up active pads

	const int vertical_semitones = row_interval_semitones(row_interval, inkey);
	if (inkey) {
		set_pad_scale_in_key (scale_root, octave, mode, origin, vertical_semitones);
	} else {
		set_pad_scale_chromatic (scale_root, octave, mode, origin, vertical_semitones);
	}

	// Write the state message for every pad

	for (auto const & p : _nn_pad_map) {
		write (p.second->state_msg ());
	}

	// Store state

	bool changed = false;

	if (_scale_root != scale_root) {
		_scale_root = scale_root;
		changed = true;
	}
	if (_root_octave != octave) {
		_root_octave = octave;
		changed = true;
	}
	if (_in_key != inkey) {
		_in_key = inkey;
		changed = true;
	}
	if (_mode != mode) {
		_mode = mode;
		changed = true;
	}
	if (_note_grid_origin != origin) {
		_note_grid_origin = origin;
		changed = true;
	}
	if (_row_interval != row_interval) {
		_row_interval = row_interval;
		changed = true;
	}

	if (changed) {
		ScaleChange (); /* EMIT SIGNAL */
	}
}

void
Push2::set_percussive_mode (bool yn)
{
	if (!yn) {
		set_pad_scale (_scale_root,
		               _root_octave,
		               _mode,
		               _note_grid_origin,
		               _row_interval,
		               _in_key);
		_percussion = false;
		return;
	}

	int drum_note = 36;

	_fn_pad_map.clear ();

	for (int row = 0; row < 8; ++row) {

		for (int col = 0; col < 4; ++col) {

			int index = 36 + (row*8) + col;
			boost::shared_ptr<Pad> pad = _nn_pad_map[index];

			pad->filtered = drum_note;
			drum_note++;
		}
	}

	for (int row = 0; row < 8; ++row) {

		for (int col = 4; col < 8; ++col) {

			int index = 36 + (row*8) + col;
			boost::shared_ptr<Pad> pad = _nn_pad_map[index];

			pad->filtered = drum_note;
			drum_note++;
		}
	}

	_percussion = true;
}

Push2Layout*
Push2::current_layout () const
{
	Glib::Threads::Mutex::Lock lm (layout_lock);
	return _current_layout;
}

void
Push2::stripable_selection_changed ()
{
	boost::shared_ptr<MidiPort> pad_port = boost::dynamic_pointer_cast<AsyncMIDIPort>(_async_in)->shadow_port();
	boost::shared_ptr<MidiTrack> current_midi_track = _current_pad_target.lock();
	boost::shared_ptr<MidiTrack> new_pad_target;
	StripableNotificationList const & selected (last_selected());

	/* See if there's a MIDI track selected */

	for (StripableNotificationList::const_iterator si = selected.begin(); si != selected.end(); ++si) {

		new_pad_target = boost::dynamic_pointer_cast<MidiTrack> ((*si).lock());

		if (new_pad_target) {
			break;
		}
	}

	if (current_midi_track != new_pad_target) {

		/* disconnect from pad port, if appropriate */

		if (current_midi_track && pad_port) {

			/* XXX this could possibly leave dangling MIDI notes.
			 *
			 * A general libardour fix is required. It isn't obvious
			 * how note resolution can be done unless disconnecting
			 * becomes "slow" (i.e. deferred for as long as it takes
			 * to resolve notes).
			 */
			current_midi_track->input()->disconnect (current_midi_track->input()->nth(0), pad_port->name(), this);
		}

		/* now connect the pad port to this (newly) selected midi
		 * track, if indeed there is one.
		 */

		if (new_pad_target && pad_port) {
			new_pad_target->input()->connect (new_pad_target->input()->nth (0), pad_port->name(), this);
			_current_pad_target = new_pad_target;
			_selection_color = get_color_index (new_pad_target->presentation_info().color());
			_contrast_color = get_color_index (Gtkmm2ext::HSV (new_pad_target->presentation_info().color()).opposite().color());
		} else {
			_current_pad_target.reset ();
			_selection_color = LED::Green;
			_contrast_color = LED::Green;
		}

		reset_pad_colors ();
	}

	TrackMixLayout* tml = dynamic_cast<TrackMixLayout*> (_track_mix_layout);
	assert (tml);

	tml->set_stripable (first_selected_stripable());
}

boost::shared_ptr<Push2::Button>
Push2::button_by_id (ButtonID bid)
{
	return _id_button_map[bid];
}

uint8_t
Push2::get_color_index (Color rgba)
{
	ColorMap::iterator i = _color_map.find (rgba);

	if (i != _color_map.end()) {
		return i->second;
	}

	double dr, dg, db, da;
	int r, g, b;
	color_to_rgba (rgba, dr, dg, db, da);
	int w = 126; /* not sure where/when we should get this value */


	r = (int) floor (255.0 * dr);
	g = (int) floor (255.0 * dg);
	b = (int) floor (255.0 * db);

	/* get a free index */

	uint8_t index;

	if (_color_map_free_list.empty()) {
		/* random replacement of any entry above zero and below 122 (where the
		 * Ableton standard colors live)
		 */
		index = 1 + (random() % 121);
	} else {
		index = _color_map_free_list.top();
		_color_map_free_list.pop();
	}

	MidiByteArray palette_msg (17,
	                           0xf0,
	                           0x00 , 0x21, 0x1d, 0x01, 0x01, 0x03, /* reset palette header */
	                           0x00, /* index = 7 */
	                           0x00, 0x00, /* r = 8 & 9 */
	                           0x00, 0x00, /* g = 10 & 11 */
	                           0x00, 0x00, /* b = 12 & 13 */
	                           0x00, 0x00, /* w (a?) = 14 & 15*/
	                           0xf7);
	palette_msg[7] = index;
	palette_msg[8] = r & 0x7f;
	palette_msg[9] = (r & 0x80) >> 7;
	palette_msg[10] = g & 0x7f;
	palette_msg[11] = (g & 0x80) >> 7;
	palette_msg[12] = b & 0x7f;
	palette_msg[13] = (b & 0x80) >> 7;
	palette_msg[14] = w & 0x7f;
	palette_msg[15] = w & 0x80;

	write (palette_msg);

	MidiByteArray update_pallette_msg (8, 0xf0, 0x00, 0x21, 0x1d, 0x01, 0x01, 0x05, 0xF7);
	write (update_pallette_msg);

	_color_map[rgba] = index;

	return index;
}

void
Push2::build_color_map ()
{
	/* These are "standard" colors that Ableton docs suggest will always be
	   there. Put them in our color map so that when we look up these
	   colors, we will use the Ableton indices for them.
	*/

	_color_map.insert (make_pair (RGB_TO_UINT (0,0,0), 0));
	_color_map.insert (make_pair (RGB_TO_UINT (204,204,204), 122));
	_color_map.insert (make_pair (RGB_TO_UINT (64,64,64), 123));
	_color_map.insert (make_pair (RGB_TO_UINT (20,20,20), 124));
	_color_map.insert (make_pair (RGB_TO_UINT (0,0,255), 125));
	_color_map.insert (make_pair (RGB_TO_UINT (0,255,0), 126));
	_color_map.insert (make_pair (RGB_TO_UINT (255,0,0), 127));

	for (uint8_t n = 1; n < 122; ++n) {
		_color_map_free_list.push (n);
	}
}

void
Push2::fill_color_table ()
{
	_colors.insert (make_pair (DarkBackground, Gtkmm2ext::rgba_to_color (0, 0, 0, 1)));
	_colors.insert (make_pair (LightBackground, Gtkmm2ext::rgba_to_color (0.98, 0.98, 0.98, 1)));

	_colors.insert (make_pair (ParameterName, Gtkmm2ext::rgba_to_color (0.98, 0.98, 0.98, 1)));

	_colors.insert (make_pair (KnobArcBackground, Gtkmm2ext::rgba_to_color (0.3, 0.3, 0.3, 1.0)));
	_colors.insert (make_pair (KnobArcStart, Gtkmm2ext::rgba_to_color (1.0, 0.0, 0.0, 1.0)));
	_colors.insert (make_pair (KnobArcEnd, Gtkmm2ext::rgba_to_color (0.0, 1.0, 0.0, 1.0)));

	_colors.insert (make_pair (KnobLineShadow, Gtkmm2ext::rgba_to_color  (0, 0, 0, 0.3)));
	_colors.insert (make_pair (KnobLine, Gtkmm2ext::rgba_to_color (1, 1, 1, 1)));

	_colors.insert (make_pair (KnobForeground, Gtkmm2ext::rgba_to_color (0.2, 0.2, 0.2, 1)));
	_colors.insert (make_pair (KnobBackground, Gtkmm2ext::rgba_to_color (0.2, 0.2, 0.2, 1)));
	_colors.insert (make_pair (KnobShadow, Gtkmm2ext::rgba_to_color (0, 0, 0, 0.1)));
	_colors.insert (make_pair (KnobBorder, Gtkmm2ext::rgba_to_color (0, 0, 0, 1)));
}

Gtkmm2ext::Color
Push2::get_color (ColorName name)
{
	Colors::iterator c = _colors.find (name);
	if (c != _colors.end()) {
		return c->second;
	}

	return random();
}

void
Push2::set_current_layout (Push2Layout* layout)
{
	if (layout && layout == _current_layout) {
		_current_layout->show ();
	} else {

		if (_current_layout) {
			_current_layout->hide ();
			_canvas->root()->remove (_current_layout);
			_previous_layout = _current_layout;
		}

		_current_layout = layout;

		if (_current_layout) {
			_canvas->root()->add (_current_layout);
			_current_layout->show ();
		}

		_canvas->request_redraw ();
	}
}

void
Push2::use_previous_layout ()
{
	if (_previous_layout) {
		set_current_layout (_previous_layout);
	}
}

void
Push2::request_pressure_mode ()
{
	MidiByteArray msg (8, 0xF0, 0x00, 0x21, 0x1D, 0x01, 0x01, 0x1F, 0xF7);
	write (msg);
}

void
Push2::set_pressure_mode (PressureMode pm)
{
	MidiByteArray msg (9, 0xF0, 0x00, 0x21, 0x1D, 0x01, 0x01, 0x1E, 0x0, 0xF7);

	switch (pm) {
	case AfterTouch:
		/* nothing to do, message is correct */
		break;
	case PolyPressure:
		msg[7] = 0x1;
		break;
	default:
		return;
	}

	write (msg);
}

boost::shared_ptr<Push2::Pad>
Push2::pad_by_xy (int x, int y)
{
	vector<boost::shared_ptr<Pad> >::size_type index = (x * 8) + y;
	if (index >= _xy_pad_map.size()) {
		return boost::shared_ptr<Pad>();
	}
	return _xy_pad_map[index];
}

boost::shared_ptr<Push2::Button>
Push2::lower_button_by_column (uint32_t col)
{
	assert (col < 8);

	switch (col) {
	case 0:
		return button_by_id (Push2::Lower1);
		break;
	case 1:
		return button_by_id (Push2::Lower2);
		break;
	case 2:
		return button_by_id (Push2::Lower3);
		break;
	case 3:
		return button_by_id (Push2::Lower4);
		break;
	case 4:
		return button_by_id (Push2::Lower5);
		break;
	case 5:
		return button_by_id (Push2::Lower6);
		break;
	case 6:
		return button_by_id (Push2::Lower7);
		break;
	case 7:
		return button_by_id (Push2::Lower8);
		break;
	}
	/*NOTREACHED*/
	return boost::shared_ptr<Push2::Button>();
}

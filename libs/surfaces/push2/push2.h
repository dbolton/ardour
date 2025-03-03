/*
 * Copyright (C) 2016-2018 Paul Davis <paul@linuxaudiosystems.com>
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

#ifndef __ardour_push2_h__
#define __ardour_push2_h__

#include <vector>
#include <map>
#include <stack>
#include <list>
#include <set>

#include <libusb.h>

#define ABSTRACT_UI_EXPORTS
#include "pbd/abstract_ui.h"

#include "midi++/types.h"

#include "ardour/mode.h"
#include "ardour/types.h"

#include "control_protocol/control_protocol.h"
#include "control_protocol/types.h"

#include "gtkmm2ext/colors.h"

#include "midi_byte_array.h"

namespace MIDI {
	class Parser;
	class Port;
}

namespace ARDOUR {
	class Port;
	class MidiBuffer;
	class MidiTrack;
}

namespace ArdourSurface {

struct Push2Request : public BaseUI::BaseRequestObject {
public:
	Push2Request () {}
	~Push2Request () {}
};

class P2GUI;
class Push2Layout;
class Push2Canvas;

class Push2 : public ARDOUR::ControlProtocol
            , public AbstractUI<Push2Request>
{
  public:
	enum ButtonID {
		TapTempo,
		Metronome,
		Upper1, Upper2, Upper3, Upper4, Upper5, Upper6, Upper7, Upper8,
		Setup,
		User,
		Delete,
		AddDevice,
		Device,
		Mix,
		Undo,
		AddTrack,
		Browse,
		Clip,
		Mute,
		Solo,
		Stop,
		Lower1, Lower2, Lower3, Lower4, Lower5, Lower6, Lower7, Lower8,
		Master,
		Convert,
		DoubleLoop,
		Quantize,
		Duplicate,
		New,
		FixedLength,
		Automate,
		RecordEnable,
		Play,
		Fwd32ndT,
		Fwd32nd,
		Fwd16thT,
		Fwd16th,
		Fwd8thT,
		Fwd8th,
		Fwd4trT,
		Fwd4tr,
		Up,
		Right,
		Down,
		Left,
		Repeat,
		Accent,
		Scale,
		Layout,
		Note,
		Session,
		OctaveUp,
		PageRight,
		OctaveDown,
		PageLeft,
		Shift,
		Select
	};

	struct LED
	{
		enum State {
			NoTransition,
			OneShot24th,
			OneShot16th,
			OneShot8th,
			OneShot4th,
			OneShot2th,
			Pulsing24th,
			Pulsing16th,
			Pulsing8th,
			Pulsing4th,
			Pulsing2th,
			Blinking24th,
			Blinking16th,
			Blinking8th,
			Blinking4th,
			Blinking2th
		};

		enum Colors {
			Black = 0,
			Red = 127,
			Green = 126,
			Blue = 125,
			DarkGray = 124,
			LightGray = 123,
			White = 122
		};

		LED (uint8_t e) : _extra (e), _color_index (Black), _state (NoTransition) {}
		virtual ~LED() {}

		uint8_t extra () const { return _extra; }
		uint8_t color_index () const { return _color_index; }
		State   state () const { return _state; }

		void set_color (uint8_t color_index);
		void set_state (State state);

		virtual MidiByteArray state_msg() const = 0;

	protected:
		uint8_t _extra;
		uint8_t _color_index;
		State   _state;
	};

	struct Pad : public LED {
		enum WhenPressed {
			Nothing,
			FlashOn,
			FlashOff,
		};

		Pad (int xx, int yy, uint8_t ex)
			: LED (ex)
			, x (xx)
			, y (yy)
			, do_when_pressed (FlashOn)
			, filtered (ex)
			, perma_color (LED::Black)
		{}

		MidiByteArray state_msg () const { return MidiByteArray (3, 0x90|_state, _extra, _color_index); }

		int coord () const { return (y * 8) + x; }
		int note_number() const { return extra(); }

		int x;
		int y;
		int do_when_pressed;
		int filtered;
		int perma_color;
	};

	struct Button : public LED {
		Button (ButtonID bb, uint8_t ex)
			: LED (ex)
			, id (bb)
			, press_method (&Push2::relax)
			, release_method (&Push2::relax)
			, long_press_method (&Push2::relax)
		{}

		Button (ButtonID bb, uint8_t ex, void (Push2::*press)())
			: LED (ex)
			, id (bb)
			, press_method (press)
			, release_method (&Push2::relax)
			, long_press_method (&Push2::relax)
		{}

		Button (ButtonID bb, uint8_t ex, void (Push2::*press)(), void (Push2::*release)())
			: LED (ex)
			, id (bb)
			, press_method (press)
			, release_method (release)
			, long_press_method (&Push2::relax)
		{}

		Button (ButtonID bb, uint8_t ex, void (Push2::*press)(), void (Push2::*release)(), void (Push2::*long_press)())
			: LED (ex)
			, id (bb)
			, press_method (press)
			, release_method (release)
			, long_press_method (long_press)
		{}

		MidiByteArray state_msg () const { return MidiByteArray (3, 0xb0|_state, _extra, _color_index); }
		int controller_number() const { return extra(); }

		ButtonID id;
		void (Push2::*press_method)();
		void (Push2::*release_method)();
		void (Push2::*long_press_method)();
		sigc::connection timeout_connection;
	};

	struct ColorButton : public Button {
		ColorButton (ButtonID bb, uint8_t ex)
			: Button (bb, ex) {}


		ColorButton (ButtonID bb, uint8_t ex, void (Push2::*press)())
			: Button (bb, ex, press) {}

		ColorButton (ButtonID bb, uint8_t ex, void (Push2::*press)(), void (Push2::*release)())
			: Button (bb, ex, press, release) {}

		ColorButton (ButtonID bb, uint8_t ex, void (Push2::*press)(), void (Push2::*release)(), void (Push2::*long_press)())
			: Button (bb, ex, press, release, long_press) {}
	};

	struct WhiteButton : public Button {
		WhiteButton (ButtonID bb, uint8_t ex)
			: Button (bb, ex) {}

		WhiteButton (ButtonID bb, uint8_t ex, void (Push2::*press)())
			: Button (bb, ex, press) {}

		WhiteButton (ButtonID bb, uint8_t ex, void (Push2::*press)(), void (Push2::*release)())
			: Button (bb, ex, press, release) {}

		WhiteButton (ButtonID bb, uint8_t ex, void (Push2::*press)(), void (Push2::*release)(), void (Push2::*long_press)())
			: Button (bb, ex, press, release, long_press) {}
	};

	enum ColorName {
		DarkBackground,
		LightBackground,

		ParameterName,
		StripableName,
		ClockText,

		KnobArcBackground,
		KnobArcStart,
		KnobArcEnd,

		KnobLine,
		KnobLineShadow,

		KnobForeground,
		KnobBackground,
		KnobShadow,
		KnobBorder,
	};

	enum PressureMode {
		AfterTouch,
		PolyPressure,
	};

  public:
	Push2 (ARDOUR::Session&);
	~Push2 ();

	static bool probe ();
	static void* request_factory (uint32_t);

	std::list<boost::shared_ptr<ARDOUR::Bundle> > bundles ();

	bool has_editor () const { return true; }
	void* get_gui () const;
	void  tear_down_gui ();

	int set_active (bool yn);
	XMLNode& get_state() const;
	int set_state (const XMLNode & node, int version);

	PBD::Signal0<void> ConnectionChange;

	boost::shared_ptr<ARDOUR::Port> input_port();
	boost::shared_ptr<ARDOUR::Port> output_port();

	int pad_note (int row, int col) const;
	PBD::Signal0<void> PadChange;

	void update_selection_color ();

	/** The "origin" or "root" of the note grid.
	 *
	 * This controls whether the grid is "fixed" in terms of the notes that it
	 * plays (so changing the scale is effectively just an overlay), or
	 * "rooted" so the root note of the scale is in the bottom left.
	 */
	enum NoteGridOrigin {
		Fixed,  ///< Bottom left pad is always C, or as close as possible
		Rooted, ///< Bottom left pad is the scale root
	};

	/** Interval between vertically adjacent note pads ("layout").
	 *
	 * The comments describe the ideal interval that is used in chromatic mode.
	 * For in-scale mode, they may be slightly adjusted, hence the more general
	 * enumerator names.
	 */
	enum RowInterval {
		Third,      /// Major third or 4 semitones
		Fourth,     /// Perfect fourth or 5 semitones
		Fifth,      /// Perfect fifth or 7 semitones
		Sequential, /// Sequential from the last row, or 8 semitones
	};

	/// "Kind" of pad that plays a note
	enum PadNoteKind { RootNote, InScaleNote, OutOfScaleNote };

	/// Set up a pad to represent a "kind" of note
	void set_pad_note_kind(Pad& pad, PadNoteKind kind);

	/** Set an "in-key" scale on the pads.
	 *
	 * "In-key" mode shows only notes which are in the scale, so every pad is
	 * in the scale and there are no "spaces".  This provides access to a wide
	 * range of notes in the scale, but no access to notes outside the scale at
	 * all.
	 *
	 *
	 * @param root The root note in the lowest octave (at most 11).
	 *
	 * @param octave The octave number of the "actual" root (at most 10).
	 *
	 * @param mode The active musical mode (scale).
	 *
	 * @param ideal_vertical_semitones The ideal interval between rows in
	 * semitones.  This is an "ideal" because it may not be possible to use
	 * exactly this interval for every row depending on the scale.  It may be
	 * bumped up to the next note in the scale if necessary, so with this mode,
	 * rows are not guaranteed to all have the same vertical interval.
	 */
	void set_pad_scale_in_key (int               root,
	                           int               octave,
	                           MusicalMode::Type mode,
	                           NoteGridOrigin    origin,
	                           int               ideal_vertical_semitones);

	/** Set a "chromatic" scale on the pads.
	 *
	 * "Chromatic" mode is chromatic from left to right, and "tuned" to some
	 * interval from bottom up, like a stringed instrument.  The default
	 * vertical interval is 5 semitones, or a perfect 4th, like strings on a
	 * bass guitar.
	 *
	 * @param root The root note in the lowest octave (at most 11).
	 *
	 * @param octave The octave number of the "actual" root (at most 10).
	 *
	 * @param mode The active musical mode (scale).
	 *
	 * @param vertical_semitones The interval between rows in semitones.  This
	 * mode guarantees that the vertical interval for all rows is always
	 * exactly this.
	 */
	void set_pad_scale_chromatic (int               root,
	                              int               octave,
	                              MusicalMode::Type mode,
	                              NoteGridOrigin    origin,
	                              int               vertical_semitones);

	void set_pad_scale (int               root,
	                    int               octave,
	                    MusicalMode::Type mode,
	                    NoteGridOrigin    origin,
	                    RowInterval       row_interval,
	                    bool              inkey);

	PBD::Signal0<void> ScaleChange;

	MusicalMode::Type mode() const { return  _mode; }
	NoteGridOrigin note_grid_origin() { return _note_grid_origin; }
	RowInterval row_interval() const { return _row_interval; }
	int scale_root() const { return _scale_root; }
	int root_octave() const { return _root_octave; }
	bool in_key() const { return _in_key; }

	Push2Layout* current_layout() const;
	void         use_previous_layout ();

	Push2Canvas* canvas() const { return _canvas; }

	enum ModifierState {
		None = 0,
		ModShift = 0x1,
		ModSelect = 0x2,
	};

	ModifierState modifier_state() const { return _modifier_state; }

	boost::shared_ptr<Button> button_by_id (ButtonID);
	static std::string button_name_by_id (ButtonID);

	void strip_buttons_off ();

	void write (const MidiByteArray&);

	uint8_t get_color_index (Gtkmm2ext::Color rgba);
	Gtkmm2ext::Color get_color (ColorName);

	PressureMode pressure_mode () const { return _pressure_mode; }
	void set_pressure_mode (PressureMode);
	PBD::Signal1<void,PressureMode> PressureModeChange;

	libusb_device_handle* usb_handle() const { return _handle; }

	ARDOUR::Session & get_session() { return *session; }

	bool stop_down () const { return _stop_down; }

	typedef std::map<int,boost::shared_ptr<Pad> > PadMap;
	PadMap const & nn_pad_map() const { return _nn_pad_map; }

	boost::shared_ptr<Pad> pad_by_xy (int x, int y);
	boost::shared_ptr<Button> lower_button_by_column (uint32_t col);

	CONTROL_PROTOCOL_THREADS_NEED_TEMPO_MAP_DECL();

  private:
	libusb_device_handle* _handle;
	bool                  _in_use;
	ModifierState         _modifier_state;

	void do_request (Push2Request*);

	int begin_using_device ();
	int stop_using_device ();
	int device_acquire ();
	void device_release ();
	int ports_acquire ();
	void ports_release ();
	void run_event_loop ();
	void stop_event_loop ();

	void relax () {}

	/* map of Buttons by CC */
	typedef std::map<int,boost::shared_ptr<Button> > CCButtonMap;
	CCButtonMap _cc_button_map;
	/* map of Buttons by ButtonID */
	typedef std::map<ButtonID,boost::shared_ptr<Button> > IDButtonMap;
	IDButtonMap _id_button_map;
	std::set<ButtonID> _buttons_down;
	std::set<ButtonID> _consumed;

	bool button_long_press_timeout (ButtonID id);
	void start_press_timeout (boost::shared_ptr<Button>, ButtonID);

	void init_buttons (bool startup);
	void init_touch_strip ();

	/* map of Pads by note number (the "fixed" note number sent by the
	 * hardware, not the note number generated if the pad is touched)
	 */
	PadMap _nn_pad_map;

	/* array of Pads by x,y duple (indexed as (x*8) + y */

	std::vector<boost::shared_ptr<Pad> > _xy_pad_map;

	/* map of Pads by note number they generate (their "filtered" value)
	 */
	typedef std::multimap<int,boost::shared_ptr<Pad> > FNPadMap;
	FNPadMap _fn_pad_map;

	void set_button_color (ButtonID, uint8_t color_index);
	void set_button_state (ButtonID, LED::State);
	void set_led_color (ButtonID, uint8_t color_index);
	void set_led_state (ButtonID, LED::State);

	void build_maps ();

	// Bundle to represent our input ports
	boost::shared_ptr<ARDOUR::Bundle> _input_bundle;
	// Bundle to represent our output ports
	boost::shared_ptr<ARDOUR::Bundle> _output_bundle;

	MIDI::Port* _input_port;
	MIDI::Port* _output_port;
	boost::shared_ptr<ARDOUR::Port> _async_in;
	boost::shared_ptr<ARDOUR::Port> _async_out;

	void connect_to_parser ();
	void handle_midi_pitchbend_message (MIDI::Parser&, MIDI::pitchbend_t);
	void handle_midi_controller_message (MIDI::Parser&, MIDI::EventTwoBytes*);
	void handle_midi_note_on_message (MIDI::Parser&, MIDI::EventTwoBytes*);
	void handle_midi_note_off_message (MIDI::Parser&, MIDI::EventTwoBytes*);
	void handle_midi_sysex (MIDI::Parser&, MIDI::byte *, size_t count);

	bool midi_input_handler (Glib::IOCondition ioc, MIDI::Port* port);

	void thread_init ();

	PBD::ScopedConnectionList session_connections;
	void connect_session_signals ();
	void notify_record_state_changed ();
	void notify_transport_state_changed ();
	void notify_loop_state_changed ();
	void notify_parameter_changed (std::string);
	void notify_solo_active_changed (bool);

	/* Button methods */
	void button_play ();
	void button_recenable ();
	void button_up ();
	void button_down ();
	void button_right ();
	void button_left ();
	void button_metronome ();
	void button_repeat ();
	void button_mute ();
	void button_solo ();
	void button_solo_long_press ();
	void button_fixed_length ();
	void button_new ();
	void button_browse ();
	void button_clip ();
	void button_session ();
	void button_undo ();
	void button_fwd32t ();
	void button_fwd32 ();
	void button_fwd16t ();
	void button_fwd16 ();
	void button_fwd8t ();
	void button_fwd8 ();
	void button_fwd4t ();
	void button_fwd4 ();
	void button_add_track ();
	void button_stop_press();
	void button_stop_release ();
	void button_stop_long_press ();
	void button_master ();
	void button_quantize ();
	void button_duplicate ();
	void button_shift_press ();
	void button_shift_release ();
	void button_shift_long_press ();
	void button_select_press ();
	void button_select_release ();
	void button_select_long_press ();
	void button_page_left ();
	void button_page_right ();
	void button_octave_up ();
	void button_octave_down ();
	void button_layout_press ();
	void button_scale_press ();
	void button_mix_press ();

	void button_upper (uint32_t n);
	void button_lower (uint32_t n);

	void button_upper_1 () { button_upper (0); }
	void button_upper_2 () { button_upper (1); }
	void button_upper_3 () { button_upper (2); }
	void button_upper_4 () { button_upper (3); }
	void button_upper_5 () { button_upper (4); }
	void button_upper_6 () { button_upper (5); }
	void button_upper_7 () { button_upper (6); }
	void button_upper_8 () { button_upper (7); }
	void button_lower_1 () { button_lower (0); }
	void button_lower_2 () { button_lower (1); }
	void button_lower_3 () { button_lower (2); }
	void button_lower_4 () { button_lower (3); }
	void button_lower_5 () { button_lower (4); }
	void button_lower_6 () { button_lower (5); }
	void button_lower_7 () { button_lower (6); }
	void button_lower_8 () { button_lower (7); }

	void start_shift ();
	void end_shift ();

	/* non-strip encoders */

	void other_vpot (int, int);
	void other_vpot_touch (int, bool);

	/* special Stripable */

	boost::shared_ptr<ARDOUR::Stripable> _master;

	sigc::connection _vblank_connection;
	bool vblank ();

	void splash ();
	PBD::microseconds_t _splash_start;

	/* the canvas */

	Push2Canvas* _canvas;

	/* Layouts */

	mutable Glib::Threads::Mutex layout_lock;
	Push2Layout* _current_layout;
	Push2Layout* _previous_layout;
	Push2Layout* _mix_layout;
	Push2Layout* _scale_layout;
	Push2Layout* _track_mix_layout;
	Push2Layout* _splash_layout;
	Push2Layout* _cue_layout;
	void set_current_layout (Push2Layout*);

	bool pad_filter (ARDOUR::MidiBuffer& in, ARDOUR::MidiBuffer& out) const;

	boost::weak_ptr<ARDOUR::MidiTrack> _current_pad_target;

	void port_registration_handler ();

	enum ConnectionState {
		InputConnected = 0x1,
		OutputConnected = 0x2
	};

	int _connection_state;

	bool connection_handler (boost::weak_ptr<ARDOUR::Port>, std::string name1, boost::weak_ptr<ARDOUR::Port>, std::string name2, bool yn);
	PBD::ScopedConnectionList port_connections;
	void connected ();

	/* GUI */

	mutable P2GUI* _gui;
	void build_gui ();

	/* pad mapping */

	void stripable_selection_changed ();

	MusicalMode::Type _mode;
	NoteGridOrigin    _note_grid_origin;
	RowInterval       _row_interval;
	int               _scale_root;
	int               _root_octave;
	bool              _in_key;
	int               _octave_shift;
	bool              _percussion;

	void restore_pad_scale ();

	void set_percussive_mode (bool);

	/* color map (device side) */

	typedef std::map<Gtkmm2ext::Color,uint8_t> ColorMap;
	typedef std::stack<uint8_t> ColorMapFreeList;
	ColorMap _color_map;
	ColorMapFreeList _color_map_free_list;
	void build_color_map ();

	/* our own colors */

	typedef std::map<ColorName,Gtkmm2ext::Color> Colors;
	Colors _colors;
	void fill_color_table ();
	void reset_pad_colors ();

	PressureMode _pressure_mode;
	void request_pressure_mode ();

	uint8_t _selection_color;
	uint8_t _contrast_color;

	bool _in_range_select;
	bool _stop_down;
};

} /* namespace */

#endif /* __ardour_push2_h__ */

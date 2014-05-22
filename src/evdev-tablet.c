/*
 * Copyright © 2014 Red Hat, Inc.
 *
 * Permission to use, copy, modify, distribute, and sell this software and
 * its documentation for any purpose is hereby granted without fee, provided
 * that the above copyright notice appear in all copies and that both that
 * copyright notice and this permission notice appear in supporting
 * documentation, and that the name of the copyright holders not be used in
 * advertising or publicity pertaining to distribution of the software
 * without specific, written prior permission.  The copyright holders make
 * no representations about the suitability of this software for any
 * purpose.  It is provided "as is" without express or implied warranty.
 *
 * THE COPYRIGHT HOLDERS DISCLAIM ALL WARRANTIES WITH REGARD TO THIS
 * SOFTWARE, INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND
 * FITNESS, IN NO EVENT SHALL THE COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * SPECIAL, INDIRECT OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER
 * RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF
 * CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */
#include "config.h"

#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include "evdev-tablet.h"

#define tablet_set_status(tablet,s) (tablet->status |= (s));
#define tablet_unset_status(tablet,s) (tablet->status &= ~(s));
#define tablet_has_status(tablet,s) (!!(tablet->status & s))

#define clip(value, minimum, maximum) (max(maximum, min(value, minimum)))

enum tablet_button_field {
	/* Each value is how many bytes away from the struct's location in the
	 * memory the field is */
	PAD_BUTTONS = 0,
	STYLUS_BUTTONS = sizeof(uint32_t)
};

static inline uint32_t
tablet_get_pressed_buttons(struct tablet_dispatch *tablet,
			   enum tablet_button_field field)
{
	uint32_t current_buttons = *(uint32_t*)(&(tablet->state) + field);
	uint32_t previous_buttons = *(uint32_t*)(&(tablet->prev_state) + field);
	return current_buttons & ~(previous_buttons);
}

static inline uint32_t
tablet_get_released_buttons(struct tablet_dispatch *tablet,
			    enum tablet_button_field field)
{
	uint32_t current_buttons = *(uint32_t*)(&(tablet->state) + field);
	uint32_t previous_buttons = *(uint32_t*)(&(tablet->prev_state) + field);
	return previous_buttons & ~(current_buttons);
}

static inline const struct input_absinfo *
tablet_get_axis(struct tablet_dispatch *tablet,
		uint32_t evcode)
{
	return libevdev_get_abs_info(tablet->device->evdev, evcode);
}

static int
tablet_add_axis(struct tablet_dispatch *tablet,
		struct evdev_device *device,
		uint32_t evcode,
		uint32_t axis)
{
	const struct input_absinfo *absinfo;

	if (!(absinfo = libevdev_get_abs_info(device->evdev, evcode)))
		return 0;

	/* TODO: implement this */

	return 1;
}

static void
tablet_process_absolute(struct tablet_dispatch *tablet,
			struct evdev_device *device,
			struct input_event *e,
			uint32_t time)
{
	switch (e->code) {
	case ABS_X:
		device->abs.x = e->value;
		tablet_set_status(tablet, TABLET_UPDATED);
		break;
	case ABS_Y:
		device->abs.y = e->value;
		tablet_set_status(tablet, TABLET_UPDATED);
		break;
	case ABS_PRESSURE:
	case ABS_DISTANCE:
	case ABS_TILT_X:
	case ABS_TILT_Y:
	case ABS_RX:
	case ABS_RY:
	case ABS_RZ:
	case ABS_WHEEL:
	case ABS_THROTTLE:
		set_bit(&tablet->axes[0], e->code);
		break;
	default:
		log_info("Unhandled ABS event code 0x%x\n", e->code);
		break;
	}
}

static void
tablet_update_tool(struct tablet_dispatch *tablet,
		   int32_t tool,
		   uint32_t enabled)
{
	assert(tool != LIBINPUT_TOOL_NONE);

	if (enabled && tool != tablet->state.tool) {
		tablet->state.tool = tool;
		tablet_set_status(tablet, TABLET_INTERACTED);
	} else if (!enabled && tool == tablet->state.tool) {
		tablet->state.tool = LIBINPUT_TOOL_NONE;
		tablet_unset_status(tablet, TABLET_INTERACTED);
	}
}

static void
tablet_update_button(struct tablet_dispatch *tablet,
		     uint32_t evcode,
		     uint32_t enable)
{
	uint32_t button, *flags;

	/* XXX: This really depends on the expected buttons fitting in the mask */
	if (evcode >= BTN_MISC && evcode <= BTN_TASK) {
		flags = &tablet->state.pad_buttons;
		button = evcode - BTN_MISC;
	} else if (evcode >= BTN_TOUCH && evcode <= BTN_STYLUS2) {
		flags = &tablet->state.stylus_buttons;
		button = evcode - BTN_TOUCH;
	} else {
		log_info("Unhandled event code %s\n",
			 libevdev_event_code_get_name(EV_KEY, evcode));
		return;
	}

	if (enable)
		(*flags) |= 1 << button;
	else
		(*flags) &= ~(1 << button);
}

static void
tablet_process_key(struct tablet_dispatch *tablet,
		   struct evdev_device *device,
		   struct input_event *e,
		   uint32_t time)
{
	switch (e->code) {
	case BTN_TOOL_PEN:
	case BTN_TOOL_RUBBER:
	case BTN_TOOL_BRUSH:
	case BTN_TOOL_PENCIL:
	case BTN_TOOL_AIRBRUSH:
	case BTN_TOOL_FINGER:
	case BTN_TOOL_MOUSE:
	case BTN_TOOL_LENS:
		/* These codes have an equivalent libinput_tool value */
		tablet_update_tool(tablet, e->code, e->value);
		break;
	case BTN_TOUCH:
		if (e->value) {
			tablet_set_status(tablet, TABLET_STYLUS_IN_CONTACT);
		} else {
			tablet_unset_status(tablet, TABLET_STYLUS_IN_CONTACT);
		}

		/* Fall through */
	case BTN_STYLUS:
	case BTN_STYLUS2:
	default:
		tablet_update_button(tablet, e->code, e->value);
		break;
	}
}

static void
tablet_process_misc(struct tablet_dispatch *tablet,
		    struct evdev_device *device,
		    struct input_event *e,
		    uint32_t time)
{
	switch (e->code) {
	case MSC_SERIAL:
		tablet->state.tool_serial = e->value;
		break;
	default:
		log_info("Unhandled MSC event code 0x%x\n", e->code);
		break;
	}
}

static void
sanitize_tablet_axes(struct tablet_dispatch *tablet)
{
	const struct input_absinfo *distance;
	const struct input_absinfo *pressure;

	distance = tablet_get_axis(tablet, ABS_DISTANCE);
	pressure = tablet_get_axis(tablet, ABS_PRESSURE);

	if (distance && pressure &&
	    bit_is_set(&tablet->axes[0], ABS_DISTANCE) &&
	    bit_is_set(&tablet->axes[0], ABS_PRESSURE) &&
	    distance->value != 0 && pressure->value != 0) {
		/* Keep distance and pressure mutually exclusive */
		clear_bit(&tablet->axes[0], ABS_DISTANCE);
	} else if (pressure && bit_is_set(&tablet->axes[0], ABS_PRESSURE) &&
		   !tablet_has_status(tablet, TABLET_STYLUS_IN_CONTACT)) {
		clear_bit(&tablet->axes[0], ABS_PRESSURE);
	}
}

static void
tablet_check_notify_tool(struct tablet_dispatch *tablet,
			 struct evdev_device *device,
			 uint32_t time,
			 bool post_check)
{
	struct libinput_device *base = &device->base;

	if (tablet->state.tool == tablet->prev_state.tool)
		return;

	if (tablet->state.tool == LIBINPUT_TOOL_NONE) {
		/* Wait for post-check */
		if (post_check)
			return;
	} else if (post_check) {
		/* Already handled in pre-check */
		return;
	}

	pointer_notify_tool_update(
		base, time, tablet->state.tool, tablet->state.tool_serial);
}

static enum libinput_pointer_axis
evcode_to_axis(uint32_t evcode)
{
	switch (evcode) {
	case ABS_DISTANCE:
		return LIBINPUT_POINTER_AXIS_DISTANCE;
	case ABS_PRESSURE:
		return LIBINPUT_POINTER_AXIS_PRESSURE;
	case ABS_TILT_X:
		return LIBINPUT_POINTER_AXIS_TILT_HORIZONTAL;
	case ABS_TILT_Y:
		return LIBINPUT_POINTER_AXIS_TILT_VERTICAL;
	default:
		return -1;
	}
}

static void
tablet_notify_axes(struct tablet_dispatch *tablet,
		   struct evdev_device *device,
		   uint32_t time)
{
	struct libinput_device *base = &device->base;
	/* A lot of the ABS axes don't apply to tablets, so we loop through the
	 * values in here so we don't waste time checking axes that will never
	 * update
	 */
	uint32_t check_axes[] = {
		ABS_DISTANCE,
		ABS_PRESSURE,
		ABS_TILT_X,
		ABS_TILT_Y
	};

	uint32_t * evcode;
	ARRAY_FOR_EACH(check_axes, evcode) {
		const struct input_absinfo * absinfo;

		if (!bit_is_set(&tablet->axes[0], *evcode))
			continue;

		absinfo = libevdev_get_abs_info(device->evdev, *evcode);

		clear_bit(&tablet->axes[0], *evcode);
		pointer_notify_axis(base, time, evcode_to_axis(*evcode),
				    absinfo->value);
	}
}

static void
tablet_notify_button_mask(struct tablet_dispatch *tablet,
			  struct evdev_device *device,
			  uint32_t time,
			  uint32_t buttons,
			  uint32_t button_base,
			  enum libinput_pointer_button_state state)
{
	struct libinput_device *base = &device->base;
	int32_t num_button = 0;

	while (buttons) {
		int enabled;

		num_button++;
		enabled = (buttons & 1);
		buttons >>= 1;

		if (!enabled)
			continue;

		pointer_notify_button(base,
				      time,
				      num_button + button_base - 1,
				      state);
	}
}

static void
tablet_notify_buttons(struct tablet_dispatch *tablet,
		      struct evdev_device *device,
		      uint32_t time,
		      bool post_check)
{
	enum libinput_pointer_button_state state;
	int32_t pad_buttons, stylus_buttons;

	if (tablet->state.pad_buttons == tablet->prev_state.pad_buttons &&
	    tablet->state.stylus_buttons == tablet->prev_state.stylus_buttons)
		return;

	if (post_check) {
		/* Only notify button releases */
		state = LIBINPUT_POINTER_BUTTON_STATE_RELEASED;
		pad_buttons = tablet_get_released_buttons(tablet, PAD_BUTTONS);
		stylus_buttons =
			tablet_get_released_buttons(tablet, STYLUS_BUTTONS);
	} else {
		/* Only notify button presses */
		state = LIBINPUT_POINTER_BUTTON_STATE_PRESSED;
		pad_buttons = tablet_get_pressed_buttons(tablet, PAD_BUTTONS);
		stylus_buttons =
			tablet_get_pressed_buttons(tablet, STYLUS_BUTTONS);
	}

	tablet_notify_button_mask(tablet, device, time,
				  pad_buttons, BTN_MISC, state);
	tablet_notify_button_mask(tablet, device, time,
				  stylus_buttons, BTN_TOUCH, state);
}

static void
tablet_flush(struct tablet_dispatch *tablet,
	     struct evdev_device *device,
	     uint32_t time)
{
	struct libinput_device *base = &device->base;
	li_fixed_t x, y;

	/* pre-update notifications */
	tablet_check_notify_tool(tablet, device, time, 0);
	tablet_notify_buttons(tablet, device, time, 0);

	if (tablet->state.tool != LIBINPUT_TOOL_NONE) {
		sanitize_tablet_axes(tablet);

		if (tablet_has_status(tablet, TABLET_UPDATED)) {
			/* FIXME: apply hysteresis, calibration */
			x = li_fixed_from_int(device->abs.x);
			y = li_fixed_from_int(device->abs.y);

			pointer_notify_motion_absolute(base, time, x, y);
			tablet_unset_status(tablet, TABLET_UPDATED);
		}

		tablet_notify_axes(tablet, device, time);
	}

	/* post-update notifications */
	tablet_notify_buttons(tablet, device, time, 1);
	tablet_check_notify_tool(tablet, device, time, 1);

	/* replace previous state */
	tablet->prev_state = tablet->state;
}

static void
tablet_process(struct evdev_dispatch *dispatch,
	       struct evdev_device *device,
	       struct input_event *e,
	       uint32_t time)
{
	struct tablet_dispatch *tablet =
		(struct tablet_dispatch *)dispatch;

	switch (e->type) {
	case EV_ABS:
		tablet_process_absolute(tablet, device, e, time);
		break;
	case EV_KEY:
		tablet_process_key(tablet, device, e, time);
		break;
	case EV_MSC:
		tablet_process_misc(tablet, device, e, time);
		break;
	case EV_SYN:
		tablet_flush(tablet, device, time);
		break;
	default:
		log_error("Unexpected event type 0x%x\n", e->type);
		break;
	}
}

static void
tablet_destroy(struct evdev_dispatch *dispatch)
{
	struct tablet_dispatch *tablet =
		(struct tablet_dispatch*)dispatch;

	free(tablet);
}

static struct evdev_dispatch_interface tablet_interface = {
	tablet_process,
	tablet_destroy
};

static void
tablet_init_axes(struct tablet_dispatch *tablet,
		 struct evdev_device *device)
{
	if (libevdev_has_event_code(device->evdev, EV_ABS, ABS_DISTANCE)) {
		tablet_add_axis(tablet, device, ABS_DISTANCE,
				LIBINPUT_POINTER_AXIS_DISTANCE);
	}

	if (libevdev_has_event_code(device->evdev, EV_ABS, ABS_PRESSURE)) {
		tablet_add_axis(tablet, device, ABS_PRESSURE,
				LIBINPUT_POINTER_AXIS_PRESSURE);
	}

	if (libevdev_has_event_code(device->evdev, EV_ABS, ABS_TILT_X) &&
	    libevdev_has_event_code(device->evdev, EV_ABS, ABS_TILT_Y)) {
		tablet_add_axis(tablet, device, ABS_TILT_X,
				LIBINPUT_POINTER_AXIS_TILT_HORIZONTAL);
		tablet_add_axis(tablet, device, ABS_TILT_Y,
				LIBINPUT_POINTER_AXIS_TILT_VERTICAL);
	}
}

static int
tablet_init(struct tablet_dispatch *tablet,
	    struct evdev_device *device)
{
	tablet->base.interface = &tablet_interface;
	tablet->device = device;
	tablet->status = TABLET_NONE;
	tablet->state.tool = LIBINPUT_TOOL_NONE;

	tablet_init_axes(tablet, device);

	return 0;
}

struct evdev_dispatch *
evdev_tablet_create(struct evdev_device *device)
{
	struct tablet_dispatch *tablet;

	tablet = zalloc(sizeof *tablet);
	if (!tablet)
		return NULL;

	if (tablet_init(tablet, device) != 0) {
		tablet_destroy(&tablet->base);
		return NULL;
	}

	return  &tablet->base;
}

#include "../common/messages.h"
#include "../common/utils.h"
#include "control.h"
#include "options.h"
#include "profiler.h"
#include "screen.h"

#include <math.h>
#include <stdlib.h>

int screen_width, screen_height, client_width, client_height,
		window_left, window_top, window_width, window_height;

struct WindowContext *contexts_list = NULL;

pthread_mutex_t active_context_lock;
struct WindowContext *active_context = NULL;

void reset_scale();
void reset_position();

void activate_window_context(xcb_window_t window) {
	if (window_get_geometry(window,
			&window_left, &window_top, &window_width, &window_height))
		window_get_root(&window);
	
	if (active_context != NULL && active_context->window == window)
		return;
	struct WindowContext *cur = contexts_list;
	while (cur != NULL) {
		if (cur->window == window) {
			active_context = cur;
			return;
		}
		cur = cur->next;
	}
	
	cur = (struct WindowContext *) malloc(sizeof (struct WindowContext));
	if (cur == NULL)
		return;
	cur->window = window;
	cur->next = contexts_list;
	contexts_list = cur;
	active_context = cur;
	
	reset_scale();
	update_frame_params();
	reset_position();
}

void apply_scale(double scale) {
	active_context->frame_width = round(client_width / scale);
	active_context->frame_height = round(client_height / scale);
}

void normalize_coord(int *coord, int frame_side, int screen_side) {
	int margin = frame_side - screen_side;
	if (margin > 0)
		*coord = -margin / 2;
	else {
		*coord = MAX(*coord, 0);
		*coord = MIN(*coord, screen_side - frame_side);
	}
}

void update_frame_params() {
	apply_scale(active_context->scale);
	if (active_context->frame_width > screen_width &&
			active_context->frame_height > screen_height) {
		double critical_scale = MIN(
			(double) client_width / screen_width,
			(double) client_height / screen_height
		);
		apply_scale(critical_scale);
	}
	
	normalize_coord(&active_context->frame_left,
			active_context->frame_width, screen_width);
	normalize_coord(&active_context->frame_top,
			active_context->frame_height, screen_height);
}


const char *label_text = NULL;
long long label_creation_time_nsec;

void show_label(const char *text) {
	label_text = text;
	label_creation_time_nsec = get_time_nsec();
}

#define LABEL_BUFFER_SIZE 256
char label_buffer[LABEL_BUFFER_SIZE];

void show_label_with_scale() {
	snprintf(label_buffer, LABEL_BUFFER_SIZE, "Scale: %.1lfx",
			active_context->scale);
	show_label(label_buffer);
}

int reset_coord(int screen_side, int frame_side,
		int window_pos, int window_side) {
	if (window_side >= frame_side)
		return window_pos;
	return window_pos - (frame_side - window_side) / 2;
}

void reset_position() {
	active_context->frame_top = reset_coord(screen_height,
			active_context->frame_height, window_top, window_height);
	active_context->frame_left = reset_coord(screen_width,
			active_context->frame_width, window_left, window_width);
}

void reset_position_handler() {
	reset_position();
	show_label("Position reset");
}

void move_up_handler() {
	active_context->frame_top -= move_step;
}

void move_down_handler() {
	active_context->frame_top += move_step;
}

void move_left_handler() {
	active_context->frame_left -= move_step;
}

void move_right_handler() {
	active_context->frame_left += move_step;
}

void toggle_window_tracking_handler() {
	window_tracking_enabled = !window_tracking_enabled;
	
	snprintf(label_buffer, LABEL_BUFFER_SIZE, "Window tracking: %s",
			window_tracking_enabled ? "On" : "Off");
	show_label(label_buffer);
}

void adjust_window_size_handler() {
	window_resize(active_context->window,
			active_context->frame_width, active_context->frame_height);
	activate_window_context(active_context->window);
	reset_position();
	
	show_label("Window adjusted");
}

void reset_scale() {
	if (window_tracking_enabled)
		active_context->scale = default_windows_scale;
	else
		active_context->scale = default_desktop_scale;
}

void reset_scale_handler() {
	reset_scale();
	show_label_with_scale();
}

void center_zoomed_position(double new_scale) {	
	int prev_width = round(client_width / active_context->scale);
	int prev_height = round(client_height / active_context->scale);
	int cur_width = round(client_width / new_scale);
	int cur_height = round(client_height / new_scale);
	active_context->frame_left += (prev_width - cur_width) / 2;
	active_context->frame_top += (prev_height - cur_height) / 2;
}

#define SCALE_EPS 1e-6

void zoom_in_handler() {
	double new_scale = active_context->scale * scale_factor;
	if (new_scale < MAX_SCALE + SCALE_EPS) {
		center_zoomed_position(new_scale);
		active_context->scale = new_scale;
	}
	
	show_label_with_scale();
}

void zoom_out_handler() {
	int prev_width = round(client_width / active_context->scale);
	int prev_height = round(client_height / active_context->scale);
	if (prev_width < screen_width || prev_height < screen_height) {
		double new_scale = active_context->scale / scale_factor;
		if (new_scale > MIN_SCALE - SCALE_EPS) {
			center_zoomed_position(new_scale);
			active_context->scale = new_scale;
		}
	}
	
	show_label_with_scale();
}

void toggle_cursor_capturing_handler() {
	cursor_capturing_enabled = !cursor_capturing_enabled;
	
	snprintf(label_buffer, LABEL_BUFFER_SIZE, "Cursor capturing: %s",
			cursor_capturing_enabled ? "On" : "Off");
	show_label(label_buffer);
}

#ifndef CONTROL_H
#define CONTROL_H

#include <xcb/xcb.h>


extern void control_screen_dimensions_set(int width, int height);
extern void control_client_dimensions_set(int width, int height);

struct WindowContext {
	xcb_window_t window;
	struct WindowContext *next;
	
	int frame_left, frame_top;
	double scale;
	int frame_width, frame_height;
	int colors_inverting_enabled;
};

extern pthread_mutex_t control_lock;
extern const struct WindowContext *control_context_get();
extern void control_context_select(xcb_window_t window);
extern void control_update_frame();


extern void control_label_get(const char **text,
		long long *creation_time_nsec);
extern void control_label_set(const char *text);

extern void reset_position_handler();
extern void move_up_handler();
extern void move_down_handler();
extern void move_left_handler();
extern void move_right_handler();
extern void move_LR_handler(signed int distance_move_screen); 
extern void move_UD_handler(signed int distance_move_screen);
extern void toggle_window_tracking_handler();
extern void adjust_window_size_handler();
extern void reset_scale_handler();
extern void zoom_in_handler();
extern void zoom_out_handler();
extern void toggle_cursor_capturing_handler();
extern void toggle_colors_inverting_handler();


#endif


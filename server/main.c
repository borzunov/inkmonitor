#include "../common/exceptions.h"
#include "../common/messages.h"
#include "../common/protocol.h"
#include "../common/utils.h"
#include "control.h"
#include "options.h"
#include "profiler.h"
#include "screen.h"
#include "security.h"
#include "shortcuts.h"
#include "transfer.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <fontconfig/fontconfig.h>
#include <errno.h>
#include <libgen.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <syslog.h>
#include <unistd.h>


#define TEXT_TITLE "RemoteInk v0.3"

#define DAEMON_NAME "remoteinkd"

const char *config_filename = "/etc/" DAEMON_NAME "/config.ini";
const char *password_filename = "/etc/" DAEMON_NAME "/passwd";

const char *lock_filename = "/var/run/" DAEMON_NAME ".pid";


void show_version() {
	printf("Copyright (c) 2013-2017 Alexander Borzunov\n");
}

void show_help(char *filename) {
	printf(
		"Server for tool for using E-Ink reader as computer monitor\n"
		"\n"
		"Usage:\n"
		"    %s [start | stop | status | kick | option]\n"
		"\n"
		"Actions:\n"
		"    start     Start the daemon\n"
		"    stop      Stop the daemon\n"
		"    status    Check wheter the daemon is running\n"
		"    kick      Ask the daemon to close the current connection\n"
		"\n"
		"Options:\n"
		"    -h, --help    Display this help and exit.\n"
		"    --version     Output version info and exit.\n",
	basename(filename));
}


ExcCode return_cursor() { //If the cursor goes out of screen of some distance, then it moves client's screen that much distance
	int x, y, same_screen;
	if (screen_cursor_get_position(&x, &y, &same_screen))
		return 0;
	const struct WindowContext *context = control_context_get();
	int new_x = MAX(x, context->frame_left);
	new_x = MIN(new_x, context->frame_left + context->frame_width);
	if (new_x != x ) {
				move_LR_handler((signed int)x-new_x); //modif reg
				screen_cursor_set_position(new_x, y); 
				return 0;}
	int new_y = MAX(y, context->frame_top);
	new_y = MIN(new_y, context->frame_top + context->frame_height);
	if (new_y != y ) {
				move_UD_handler((signed int)y-new_y); //modif reg
				screen_cursor_set_position(x, new_y); 
				return 0;}
	if (!same_screen || x != new_x || y != new_y)
		screen_cursor_set_position(new_x, new_y);
	return 0;
}

ExcCode image_expand(Imlib_Image *image, int dest_width, int dest_height) {
	imlib_context_set_image(*image);
	int source_width = imlib_image_get_width();
	int source_height = imlib_image_get_height();
	if (source_width == dest_width && source_height == dest_height)
		return 0;
	Imlib_Image dest_image = imlib_create_image(dest_width, dest_height);
	if (dest_image == NULL)
		PANIC(ERR_IMAGE);
	
	imlib_context_set_image(dest_image);
	imlib_context_set_color(0, 0, 0, 255);
	imlib_image_fill_rectangle(0, 0, dest_width, dest_height);
	imlib_blend_image_onto_image(*image, 0, 0, 0, source_width, source_height,
			(dest_width - source_width) / 2, (dest_height - source_height) / 2,
			source_width, source_height);
	
	imlib_context_set_image(*image);
	imlib_free_image_and_decache();
	*image = dest_image;
	return 0;
}

ExcCode image_resize(Imlib_Image *image, int dest_width, int dest_height) {
	imlib_context_set_image(*image);
	int source_width = imlib_image_get_width();
	int source_height = imlib_image_get_height();
	if (source_width == dest_width && source_height == dest_height)
		return 0;
	Imlib_Image dest_image = imlib_create_cropped_scaled_image(
			0, 0, source_width, source_height, dest_width, dest_height);
	imlib_free_image_and_decache();
	if (dest_image == NULL)
		PANIC(ERR_IMAGE);
	*image = dest_image;
	return 0;
}

ExcCode image_invert_colors(Imlib_Image image) {
	imlib_context_set_image(image);
	unsigned *data = imlib_image_get_data();
	int data_length = imlib_image_get_width() * imlib_image_get_height();
	for (int i = 0; i < data_length; i++)
		data[i] = 0xffffff - data[i];
	imlib_image_put_back_data(data);
	return 0;
}

#define LABEL_SHOW_TIME_NSEC (2 * (NSECS_PER_SEC))

#define LABEL_BOX_MARGIN 25
#define LABEL_BOX_PADDING 5

ExcCode draw_label(Imlib_Image image) {
	const char *text;
	long long creation_time_nsec;
	control_label_get(&text, &creation_time_nsec);
	if (text == NULL)
		return 0;
	if (get_time_nsec() - creation_time_nsec > LABEL_SHOW_TIME_NSEC) {
		text = NULL;
		return 0;
	}
	
	int label_width, label_height;
	imlib_get_text_size(text, &label_width, &label_height);
	imlib_context_set_image(image);
	int box_left = LABEL_BOX_MARGIN;
	int box_top = imlib_image_get_height() -
			LABEL_BOX_MARGIN - 2 * LABEL_BOX_PADDING - label_height;
	int box_width = 2 * LABEL_BOX_PADDING + label_width;
	int box_height = 2 * LABEL_BOX_PADDING + label_height;
	imlib_context_set_color(255, 255, 255, 255);
	imlib_image_fill_rectangle(box_left, box_top, box_width, box_height);
	imlib_context_set_color(0, 0, 0, 255);
	imlib_image_draw_rectangle(box_left, box_top, box_width, box_height);
	imlib_text_draw(box_left + LABEL_BOX_PADDING, box_top + LABEL_BOX_PADDING,
			text);
	return 0;
}

ExcCode image_turn_to_data(Imlib_Image image, unsigned **res) {
	// Note: *res will be set to pointer to a buffer that contains image
	//       pixels encoded as RGBX. This pointer should be freed by caller.
	imlib_context_set_image(image);
	size_t data_size = imlib_image_get_width() * imlib_image_get_height() *
			sizeof (unsigned);
	*res = malloc(data_size);
	if (*res == NULL)
		PANIC(ERR_MALLOC);
	unsigned *data = imlib_image_get_data_for_reading_only();
	memcpy(*res, data, data_size);
	imlib_free_image_and_decache();
	return 0;
}

void track_focused_window() {
	if (control_context_get() != NULL)
		control_update_frame();
	
	xcb_window_t window;
	if (!window_tracking_enabled || screen_get_focused(&window))
		screen_get_root(&window);
	control_context_select(window);
}

ExcCode get_screenshot_data(unsigned **image_data,
		int client_width, int client_height) {
	track_focused_window();
	if (cursor_capturing_enabled)
		TRY(return_cursor());
	const struct WindowContext *context = control_context_get();
	Imlib_Image image;
	TRY(screen_shot(
			context->frame_left, context->frame_top,
			context->frame_width, context->frame_height,
			&image));
	TRY(image_expand(&image, context->frame_width, context->frame_height));
	TRY(image_resize(&image, client_width, client_height));
	if (context->colors_inverting_enabled)
		TRY(image_invert_colors(image));
	TRY(draw_label(image));
	TRY(image_turn_to_data(image, image_data));
	return 0;
}

ExcCode get_screenshot_data_checked(unsigned **image_data,
		int client_width, int client_height, int conn_fd) {
	if (get_screenshot_data(image_data, client_width, client_height)) {
		transfer_send_error(conn_fd, exc_message);
		return -1;
	}
	return 0;
}


int serv_fd, conn_fd;

int has_stats = 0;

void defer_unlock_control_lock() {
	pthread_mutex_unlock(&control_lock);
}

ExcCode send_first_frame(unsigned **image_data,
		int client_width, int client_height) {
	pthread_mutex_lock(&control_lock);
	push_defer(defer_unlock_control_lock);
	
	TRY_WITH_DEFER(get_screenshot_data_checked(
			image_data, client_width, client_height, conn_fd));
	TRY_WITH_DEFER(transfer_image_send_all(
			conn_fd, *image_data, client_width, client_height));
	
	pop_defer(defer_unlock_control_lock);
	return 0;
}

char conn_check_char = CONN_CHECK;

ExcCode send_next_frame(unsigned **image_data,
		int client_width, int client_height,
		int region_width, int region_height) {
	// Check whether connection is dead. If so, SIGPIPE will be sent.
	if (write(conn_fd, &conn_check_char, sizeof (char)) < 0)
		PANIC(ERR_SOCK_TRANSFER);
	
	pthread_mutex_lock(&control_lock);
	push_defer(defer_unlock_control_lock);
	
	profiler_start(STAGE_SHOT);
	unsigned *next_image_data;
	TRY_WITH_DEFER(get_screenshot_data_checked(
			&next_image_data, client_width, client_height, conn_fd));
	profiler_finish(STAGE_SHOT);
	
	unsigned i, j;
	for (i = 0; i < height_divisor; i++)
		for (j = 0; j < width_divisor; j++)
			TRY_WITH_DEFER(transfer_image_send_diff(
				conn_fd, *image_data, next_image_data,
				client_width, client_height,
				j * region_width, i * region_height,
				region_width, region_height
			));
	free(*image_data);
	*image_data = next_image_data;

	profiler_traffic_count_uncompressed(client_width * client_height * 4);
	has_stats = 1;
	
	pop_defer(defer_unlock_control_lock);
	return 0;
}


enum State {STATE_SHUTDOWN, STATE_LISTEN, STATE_HANDLE_CLIENT};
volatile sig_atomic_t state = STATE_LISTEN;


#define ERR_CLIENT_VERSION "Incompatible client version"

#define TRANSFER_BUFFER_SIZE 256
char transfer_buffer[TRANSFER_BUFFER_SIZE];

ExcCode client_handshake(int *client_width, int *client_height) {
	const char *line;
	TRY(transfer_recv_string(conn_fd, &line));
	if (strcmp(line, HEADER)) {
		transfer_send_error(conn_fd, ERR_CLIENT_VERSION);
		PANIC(ERR_CLIENT_VERSION);
	}
		
	TRY(transfer_recv_string(conn_fd, &line));
	int is_correct;
	TRY(security_check_password(line, &is_correct));
	if (!is_correct) {
		transfer_send_error(conn_fd, ERR_WRONG_PASSWORD);
		PANIC(ERR_WRONG_PASSWORD);
	}
		
	if (read(conn_fd, transfer_buffer, COORD_SIZE * 2) != COORD_SIZE * 2)
		PANIC(ERR_SOCK_TRANSFER);
	int i = -1;
	READ_COORD(*client_width, transfer_buffer, i);
	READ_COORD(*client_height, transfer_buffer, i);
	return 0;
}


ExcCode screen_of_display(xcb_connection_t *c, int screen,
		xcb_screen_t **res) {
	xcb_screen_iterator_t iter = xcb_setup_roots_iterator(xcb_get_setup(c));
	for (int i = 0; iter.rem; i++) {
		if (i == screen) {
			*res = iter.data;
			return 0;
		}
		xcb_screen_next(&iter);
	}
	PANIC(ERR_X_REQUEST, "screen_of_display");
}

void *handle_shortcuts(void *arg) {
	if (shortcuts_handle_start(shortcuts)) {
		syslog(LOG_CRIT, "%s", exc_message);
		
		state = STATE_SHUTDOWN;
		close(serv_fd);
	}
	return NULL;
}

FcPattern *font_pattern = 0;
FcPattern *font_match = 0;
FcChar8 *font_dirname = 0;
FcChar8 *font_basename = 0;

void remove_extension(char *filename) {
	for (int i = strlen(filename) - 1; i != -1; i--)
		if (filename[i] == '.') {
			filename[i] = 0;
			return;
		}
}

void defer_for_load_fonts() {
	if (font_pattern)
		FcPatternDestroy(font_pattern);
	if (font_match)
		FcPatternDestroy(font_match);
	if (font_dirname)
		FcStrFree(font_dirname);
	if (font_basename)
		FcStrFree(font_basename);
}

#define FONT_IMLIB_REPR_SIZE 256
char font_imlib_repr[FONT_IMLIB_REPR_SIZE];

ExcCode load_fonts() {
	push_defer(defer_for_load_fonts);
	
	if (FcInit() == FcFalse)
		PANIC_WITH_DEFER(ERR_FONT, font_pattern_repr);
	font_pattern = FcNameParse((const FcChar8 *) font_pattern_repr);
	if (FcConfigSubstitute(0, font_pattern, FcMatchPattern) == FcFalse)
		PANIC_WITH_DEFER(ERR_FONT, font_pattern_repr);
	FcDefaultSubstitute(font_pattern);
	FcResult result;
	font_match = FcFontMatch(0, font_pattern, &result);
	
	FcChar8 *filename;
	if (FcPatternGetString(font_match, FC_FILE, 0, &filename) != FcResultMatch)
		PANIC_WITH_DEFER(ERR_FONT, font_pattern_repr);
	font_dirname = FcStrDirname(filename);
	font_basename = FcStrBasename(filename);
	double size;
	if (FcPatternGetDouble(font_match, FC_SIZE, 0, &size) != FcResultMatch)
		PANIC_WITH_DEFER(ERR_FONT, font_pattern_repr);
	
	imlib_add_path_to_font_path((char *) font_dirname);
	remove_extension((char *) font_basename);
	snprintf(font_imlib_repr, FONT_IMLIB_REPR_SIZE, "%s/%.0lf",
			(char *) font_basename, size);
	Imlib_Font label_font = imlib_load_font(font_imlib_repr);
	if (label_font == NULL)
		PANIC_WITH_DEFER(ERR_FONT, font_pattern_repr);
	imlib_context_set_font(label_font);
	
	pop_defer(defer_for_load_fonts);
	return 0;
}

xcb_connection_t *display;
pthread_t shortcuts_thread;

#define ERR_SCREEN_DEPTH "Your screen has %d-bit color depth, " \
		"but only 24-bit depth is supported yet"

ExcCode server_create() {	
	int default_screen_no;
	display = xcb_connect(NULL, &default_screen_no);
	if (xcb_connection_has_error(display))
		PANIC(ERR_X_CONNECT);
	xcb_screen_t *screen;
	TRY(screen_of_display(display, default_screen_no, &screen));
	int screen_width = screen->width_in_pixels;
	int screen_height = screen->height_in_pixels;
	xcb_window_t root = screen->root;
	control_screen_dimensions_set(screen_width, screen_height);
	
	int depth = screen->root_depth;
	if (depth != 24)
		PANIC(ERR_SCREEN_DEPTH, depth);
	
	TRY(screen_init(display, screen, root));
	TRY(shortcuts_init(display, screen, root));
	
	TRY(options_config_load(config_filename));
	syslog(LOG_DEBUG,
			"Monitor resolution: %dx%d", screen_width, screen_height);
	
	load_fonts();
	
	if (pthread_mutex_init(&control_lock, NULL))
		PANIC(ERR_MUTEX_INIT);
	int shortcuts_res;	
	if (pthread_create(&shortcuts_thread, NULL,
			handle_shortcuts, &shortcuts_res))
		PANIC(ERR_THREAD_CREATE);

	serv_fd = socket(AF_INET, SOCK_STREAM, 0);
	if (serv_fd < 0)
		PANIC(ERR_SOCK_CREATE);
	return 0;
}

void server_destroy() {
	close(serv_fd);
	
	shortcuts_handle_stop();
	
	imlib_free_font();
	
	shortcuts_free();
	screen_free();
	
	xcb_disconnect(display);
	
	pthread_cancel(shortcuts_thread);
	pthread_mutex_destroy(&control_lock);
	
	if (has_stats && stats_enabled) {
		if (profiler_save(stats_filename))
			syslog(LOG_ERR, "%s", exc_message);
		else
			syslog(LOG_DEBUG, "Stats saved to \"%s\"", stats_filename);
	}
}

ExcCode server_setup() {
	profiler_traffic_init();
	
	struct hostent *serv = gethostbyname(server_host);
	if (serv == NULL)
		PANIC(ERR_SOCK_RESOLVE, server_host);
	struct sockaddr_in serv_addr;
	serv_addr.sin_family = AF_INET;
	memcpy(&serv_addr.sin_addr.s_addr, serv->h_addr, serv->h_length);
	serv_addr.sin_port = htons(server_port);
	
	int optval = 1;
	size_t optlen = sizeof(int);
	if (setsockopt(serv_fd, SOL_SOCKET, SO_REUSEADDR, &optval, optlen))
		PANIC(ERR_SOCK_CONFIG);
	if (bind(serv_fd, (struct sockaddr *) &serv_addr, sizeof (serv_addr)))
		PANIC(ERR_SOCK_BIND, server_host, server_port);
	if (listen(serv_fd, 5))
		PANIC(ERR_SOCK_LISTEN, server_host, server_port);
	return 0;
}

#define CLIENT_ADDR_BUFFER_SIZE 256
char client_addr_buffer[CLIENT_ADDR_BUFFER_SIZE];

void defer_close_conn_fd() {
	close(conn_fd);
}

ExcCode server_handle_client(const struct sockaddr_in *client_addr) {
	push_defer(defer_close_conn_fd);
	
	syslog(LOG_INFO, "Accepted client connection from %s",
			inet_ntoa(client_addr->sin_addr));
	int client_width, client_height;
	TRY_WITH_DEFER(client_handshake(&client_width, &client_height));
	control_client_dimensions_set(client_width, client_height);
	syslog(LOG_DEBUG,
			"Reader resolution: %dx%d", client_width, client_height);
	
	control_label_set("Connected");
	
	unsigned *image_data;
	TRY_WITH_DEFER(send_first_frame(&image_data, client_width, client_height));
	
	unsigned region_width = client_width / width_divisor;
	unsigned region_height = client_height / height_divisor;
	while (state == STATE_HANDLE_CLIENT) {
		long long frame_start_time = get_time_nsec();
		
		TRY_WITH_DEFER(send_next_frame(&image_data,
				client_width, client_height, region_width, region_height));

		long long frame_duration = get_time_nsec() - frame_start_time;
		long long sleep_time = NSECS_PER_SEC / max_fps - frame_duration;
		if (sleep_time > 0)
			usleep(sleep_time / NSECS_PER_USEC);
		else
		if (stats_enabled && sleep_time < 0)
			syslog(LOG_DEBUG, "Too slow! Frame duration is %lld ms",
					frame_duration / NSECS_PER_MSEC);
	}
	syslog(LOG_INFO, "Client disconnected");
	
	pop_defer(defer_close_conn_fd);
	return 0;
}


void fatal_handler(int code) {
	pop_all_defers();
	
	syslog(LOG_NOTICE, "Caught signal \"%s\", shutted down", strsignal(code));
	exit(EXIT_SUCCESS);
}

void term_handler(int code) {
	pop_all_defers();
	
	syslog(LOG_INFO, "Server shutted down");
	exit(EXIT_SUCCESS);
}

void kick_handler(int code) {
	if (state == STATE_HANDLE_CLIENT) {
		state = STATE_LISTEN;
		close(conn_fd);
		
		syslog(LOG_ERR, "Client has kicked");
	}
}

void broken_pipe_handler(int code) {
	if (state == STATE_HANDLE_CLIENT) {
		state = STATE_LISTEN;
		close(conn_fd);
		
		syslog(LOG_ERR, ERR_SOCK_TRANSFER);
	}
}

const int signals_ignored[] = {
	SIGALRM, SIGTSTP, SIGTTIN, SIGTTOU, SIGURG, SIGXCPU, SIGXFSZ, SIGVTALRM,
	SIGPROF, SIGIO, SIGUSR1, SIGUSR2, SIGHUP, 0
};

const int signals_fatal[] = {
	SIGQUIT, SIGILL, SIGTRAP, SIGABRT, SIGIOT, SIGBUS, SIGFPE, SIGSEGV,
	SIGSTKFLT, SIGPWR, SIGSYS, SIGCONT, 0
};

ExcCode configure_signal_handlers() {
	struct sigaction action;
	sigemptyset(&action.sa_mask);
	action.sa_flags = 0;
	
	action.sa_handler = SIG_IGN;
	for (int i = 0; signals_ignored[i]; i++)
		if (sigaction(signals_ignored[i], &action, NULL))
			PANIC(ERR_SIGNAL_CONFIGURE);
	
	action.sa_handler = fatal_handler;
	for (int i = 0; signals_fatal[i]; i++)
		if (sigaction(signals_fatal[i], &action, NULL))
			PANIC(ERR_SIGNAL_CONFIGURE);
	
	action.sa_handler = term_handler;
	sigaction(SIGTERM, &action, NULL);
	
	action.sa_handler = kick_handler;
	sigaction(SIGINT, &action, NULL);
	
	action.sa_handler = broken_pipe_handler;
	sigaction(SIGPIPE, &action, NULL);
	return 0;
}

ExcCode daemon_main() {
	TRY(server_create());
	push_defer(server_destroy);
	
	TRY_WITH_DEFER(server_setup());
	syslog(LOG_INFO, "Server listens on %s:%d", server_host, server_port);
	while (state == STATE_LISTEN || state == STATE_HANDLE_CLIENT) {		
		struct sockaddr_in client_addr;
		socklen_t client_len = sizeof (struct sockaddr_in);
		conn_fd = accept(serv_fd, (struct sockaddr *) &client_addr,
				&client_len);
		if (conn_fd < 0)
			PANIC_WITH_DEFER(ERR_SOCK_ACCEPT);	
		state = STATE_HANDLE_CLIENT;
		if (server_handle_client(&client_addr) && state > STATE_LISTEN) {
			state = STATE_LISTEN;
			syslog(LOG_ERR, "%s", exc_message);
		}
	}
	
	pop_defer(server_destroy);
	return 0;
}

#define ERR_LOCK_FILE_CONTENT "Lock file \"%s\" doesn't contain PID"
#define ERR_LOCK_ALREADY_RUNNING \
		"It appears the daemon is already running with PID %d"
#define ERR_LOCK_ISNT_RUNNING "The daemon isn't running"
#define ERR_LOCK_DEFUNCT "Lock file \"%s\" has been detected. It appears "\
		"it's owned by the process with PID %d, which is now defunct. "\
		"Delete the lock file (you should run \"sudo rm %s\") and try again."
#define ERR_LOCK_ACQUIRE "Failed to acquire exclusive lock on lock file \"%s\""

#define ERR_DAEMON_START "Failed to start daemon"

#define ERR_PROCESS_CHECK "Failed to check existence of process with PID %d"
#define ERR_SIGNAL_SEND "Failed to send signal to process with PID %d"

#define PID_BUFFER_SIZE 256
char pid_buffer[PID_BUFFER_SIZE];

enum SendSignalResult {
	SEND_FAIL_OPEN_LOCK, SEND_INCORRECT_LOCK_FILE_CONTENT, SEND_DEFUNCT,
	SEND_FAIL_SEND_SIGNAL, SEND_OK
};

FILE *lock_file;

void defer_fclose_lock_file() {
	fclose(lock_file);
}

enum SendSignalResult send_signal_to_daemon(int signal_code, pid_t *lock_pid) {	
	lock_file = fopen(lock_filename, "r");
	if (lock_file == NULL)
		return SEND_FAIL_OPEN_LOCK;
	push_defer(defer_fclose_lock_file);
	
	if (fscanf(lock_file, "%d", lock_pid) != 1)
		return SEND_INCORRECT_LOCK_FILE_CONTENT;
		
	pop_defer(defer_fclose_lock_file);
	
	// Check whether process with PID from the lock file is running
	if (kill(*lock_pid, signal_code)) {
		if (errno == ESRCH)
			return SEND_DEFUNCT;
		return SEND_FAIL_SEND_SIGNAL;
	}
	return SEND_OK;
}

int lock_fd;

void defer_close_and_unlink_lock_file() {
	close(lock_fd);
	unlink(lock_filename);
}

ExcCode run_daemon() {
	// Load password beforehand
	TRY(security_load_password(password_filename));
	
	// Try to grab the lock file
	lock_fd = open(lock_filename, O_RDWR | O_CREAT | O_EXCL, 0644);
	if (lock_fd == -1) {
		pid_t lock_pid;
		switch (send_signal_to_daemon(0, &lock_pid)) {
			case SEND_FAIL_OPEN_LOCK:
				PANIC(ERR_FILE_CREATE, lock_filename);
			case SEND_INCORRECT_LOCK_FILE_CONTENT:
				PANIC(ERR_LOCK_FILE_CONTENT, lock_filename);
			case SEND_DEFUNCT:
				PANIC(ERR_LOCK_DEFUNCT,
						lock_filename, lock_pid, lock_filename);
			case SEND_FAIL_SEND_SIGNAL:
				PANIC(ERR_PROCESS_CHECK, lock_pid);
			case SEND_OK:
				PANIC(ERR_LOCK_ALREADY_RUNNING, lock_pid);
		}
	}
	
	push_defer(defer_close_and_unlink_lock_file);
	
	// Set a lock on the lock file
	if (lockf(lock_fd, F_TLOCK, 0) < 0)
		PANIC_WITH_DEFER(ERR_LOCK_ACQUIRE, lock_filename);
		
	// Set our current working directory to root to avoid tying up
	// any directories
	if (chdir("/") < 0)
		PANIC_WITH_DEFER(ERR_DAEMON_START);
	
	// Move ourselves into the background and become a daemon.
	// Open file descriptors among others are inherited here.
	switch (fork()) {
		case 0:
			break;
		case -1:
			PANIC_WITH_DEFER(ERR_DAEMON_START);
			break;
		default:
			exit(EXIT_SUCCESS);
			break;
	}
	
	// Make the process a session and process group leader
	if (setsid() < 0)
		PANIC_WITH_DEFER(ERR_DAEMON_START);
		
	// Restrict file creation mode to 640 for security purposes
	umask(0137);
	
	configure_signal_handlers();
	
	// Save PID
	int pid = getpid();
	snprintf(pid_buffer, PID_BUFFER_SIZE, "%d\n", pid);
	if (write(lock_fd, pid_buffer, strlen(pid_buffer)) < 0)
		PANIC_WITH_DEFER(ERR_FILE_WRITE, lock_filename);
		
	printf("[+] " DAEMON_NAME " started (PID %d)\n", pid);
	
	// Close unnecessary resources
	for (int i = getdtablesize(); i >= 0; i--)
		if (i != lock_fd)
			close(i);

	// Restore standard I/O file descriptors
	int stdio_fd = open("/dev/null", O_RDWR);
	if (stdio_fd == -1)
		PANIC_WITH_DEFER(ERR_DAEMON_START);
	if (dup(stdio_fd) == -1)
		PANIC_WITH_DEFER(ERR_DAEMON_START);
	if (dup(stdio_fd) == -1)
		PANIC_WITH_DEFER(ERR_DAEMON_START);
	
	openlog(DAEMON_NAME, LOG_CONS, LOG_LOCAL0);
	
	if (daemon_main() && state > STATE_SHUTDOWN) {
		state = STATE_SHUTDOWN;
		syslog(LOG_CRIT, "%s", exc_message);
		
		closelog();
		pop_defer(defer_close_and_unlink_lock_file);
		exit(EXIT_FAILURE);
	}
	
	closelog();
	pop_defer(defer_close_and_unlink_lock_file);
	return 0;
}

ExcCode send_action_to_daemon(int signal_code, pid_t *lock_pid) {
	switch (send_signal_to_daemon(signal_code, lock_pid)) {
		case SEND_FAIL_OPEN_LOCK:
			PANIC(ERR_LOCK_ISNT_RUNNING);
		case SEND_INCORRECT_LOCK_FILE_CONTENT:
			PANIC(ERR_LOCK_FILE_CONTENT, lock_filename);
		case SEND_DEFUNCT:
			PANIC(ERR_LOCK_DEFUNCT, lock_filename, *lock_pid, lock_filename);
		case SEND_FAIL_SEND_SIGNAL:
			PANIC(ERR_SIGNAL_SEND, *lock_pid);
		default:
			return 0;
	}
}

ExcCode check_whether_daemon_running(int *res, pid_t *lock_pid) {
	switch (send_signal_to_daemon(0, lock_pid)) {
		case SEND_FAIL_OPEN_LOCK:
			*res = 0;
			return 0;
		case SEND_INCORRECT_LOCK_FILE_CONTENT:
			PANIC(ERR_LOCK_FILE_CONTENT, lock_filename);
		case SEND_DEFUNCT:
			PANIC(ERR_LOCK_DEFUNCT, lock_filename, *lock_pid, lock_filename);
		case SEND_FAIL_SEND_SIGNAL:
			PANIC(ERR_PROCESS_CHECK, *lock_pid);
		default:
			*res = 1;
			return 0;
	}
}

#define ERR_ARG_EXCESS "Excess argument is present (see --help)"
#define ERR_ACTION_UNSPECIFIED "You need to specify action (see --help)"
#define ERR_ACTION_UNKNOWN "Unknown action \"%s\" (see --help)"

ExcCode perform_action(int argc, char *argv[]) {
	printf(TEXT_TITLE " - Server\n");
	
	if (argc < 2)
		PANIC(ERR_ACTION_UNSPECIFIED);
	if (argc > 2)
		PANIC(ERR_ARG_EXCESS);
	const char *action = argv[1];
	
	if (!strcmp(action, "-h") || !strcmp(action, "--help")) {
		show_help(argv[0]);
		exit(EXIT_SUCCESS);
	}
	if (!strcmp(action, "--version")) {
		show_version();
		exit(EXIT_SUCCESS);
	}
	
	pid_t pid;
	if (!strcmp(action, "start")) {
		TRY(run_daemon());
	} else
	if (!strcmp(action, "stop")) {
		TRY(send_action_to_daemon(SIGTERM, &pid));
		printf("[+] " DAEMON_NAME " stopped (PID %d)\n", pid);
	} else
	if (!strcmp(action, "status")) {
		int is_running;
		TRY(check_whether_daemon_running(&is_running, &pid));
		if (is_running)
			printf("[*] " DAEMON_NAME " is running (PID %d)\n", pid);
		else
			printf("[*] " DAEMON_NAME " isn't running\n");
	} else
	if (!strcmp(action, "kick")) {
		TRY(send_action_to_daemon(SIGINT, &pid));
		printf("[+] " DAEMON_NAME " (PID %d) "
				"asked to close the current connection\n", pid);
	} else
	if (!strcmp(action, "passwd")) {
		TRY(security_change_password(password_filename));
		printf("[+] Password updated (restart the daemon to apply changes)\n");
	} else
		PANIC(ERR_ACTION_UNKNOWN, action);
	return 0;
}

int main(int argc, char *argv[]) {
	if (perform_action(argc, argv)) {
		fprintf(stderr, "[-] Error: %s\n", exc_message);
		return EXIT_FAILURE;
	}
	return EXIT_SUCCESS;
}

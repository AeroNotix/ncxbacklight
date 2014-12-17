/*
 * NCXBacklight - Commandline ncurses interface to xrandr backlight capabilities
 * Copyright (C) 2014 Brandon Surmanski
 */

#include <stdlib.h>
#include <stddef.h>
#include <string.h>
#include <ncurses.h>
#include <xcb/xcb.h>
#include <xcb/xcb_util.h>
#include <xcb/xproto.h>
#include <xcb/randr.h>
#include <sys/signal.h>
#include <unistd.h>

WINDOW *window;

typedef struct {
    xcb_randr_output_t output;
    long value;
    long min;
    long max;
} ncxb_output_t;

typedef struct {
    xcb_window_t window;
    long selected; //selected output
    long noutputs;
    ncxb_output_t *outputs;
} ncxb_screen_t;

static xcb_atom_t backlight;
static xcb_connection_t *conn = NULL;
static ncxb_screen_t *screens;
static int width = 0;
static int height = 0;
static bool ncxb_clear = false;

static void ncxb_set(xcb_connection_t *conn, xcb_randr_output_t output, long value) {
    xcb_randr_change_output_property(conn, output, backlight, XCB_ATOM_INTEGER, 32,
            XCB_PROP_MODE_REPLACE, 1, (unsigned char*) &value);
}

static long ncxb_get(xcb_connection_t *conn, xcb_randr_output_t output) {
    xcb_generic_error_t *error;
    xcb_randr_get_output_property_cookie_t prop_cookie;
    xcb_randr_get_output_property_reply_t *prop_reply = NULL;
    long value;

    if(backlight != XCB_ATOM_NONE) {
        prop_cookie = xcb_randr_get_output_property(conn, output, backlight,
                                                    XCB_ATOM_NONE,
                                                    0, 4, 0, 0);
        prop_reply = xcb_randr_get_output_property_reply(conn, prop_cookie, &error);
    }

    if(prop_reply == NULL ||
            prop_reply->type != XCB_ATOM_INTEGER ||
            prop_reply->num_items != 1 ||
            prop_reply->format != 32) {
        value = -1;
    } else {
        value = *((int32_t*) xcb_randr_get_output_property_data(prop_reply));
    }

    free(prop_reply);

    return value;
}

// backlight range; min and max
static void ncxb_range(xcb_connection_t *conn, xcb_randr_output_t output, int *min, int *max) {
    xcb_generic_error_t *error;
    xcb_randr_query_output_property_cookie_t prop_cookie;
    xcb_randr_query_output_property_reply_t *prop_reply = NULL;

    prop_cookie = xcb_randr_query_output_property(conn, output, backlight);
    prop_reply = xcb_randr_query_output_property_reply(conn, prop_cookie, &error);

    // could not get range
    if(error || !prop_reply) goto CONTINUE;

    if(prop_reply->range &&
            xcb_randr_query_output_property_valid_values_length(prop_reply) == 2) {
        int32_t *values = xcb_randr_query_output_property_valid_values(prop_reply);

        if(min) *min = values[0];
        if(max) *max = values[1];
    }

CONTINUE:
    free(prop_reply);
}

static xcb_atom_t ncxb_get_backlight_atom(xcb_connection_t *conn) {
    xcb_atom_t ret;
    xcb_generic_error_t *error;
    xcb_intern_atom_cookie_t backlight_cookie[2];
    backlight_cookie[0] = xcb_intern_atom(conn, 1, strlen("Backlight"), "Backlight");
    backlight_cookie[1] = xcb_intern_atom(conn, 1, strlen("BACKLIGHT"), "BACKLIGHT");
    xcb_intern_atom_reply_t *backlight_reply = xcb_intern_atom_reply(conn, backlight_cookie[0], &error);
    if(error || !backlight_reply) goto CONTINUE;
    ret = backlight_reply->atom;

    if(ret == XCB_NONE) {
        free(backlight_reply);
        xcb_intern_atom_reply_t *backlight_reply = xcb_intern_atom_reply(conn, backlight_cookie[1], &error);
        if(error || !backlight_reply) goto CONTINUE;
        ret = backlight_reply->atom;
    }

CONTINUE:
    free(backlight_reply);
    return ret;
}

void ncxb_exit(void);
void ncxb_handle_signal(int sig) {
    ncxb_exit();
    exit(0);
}

unsigned ncxb_count_screens(xcb_connection_t *conn) {
    unsigned ret = 0;
    xcb_screen_iterator_t iter;
    iter = xcb_setup_roots_iterator(xcb_get_setup(conn));
    while(iter.rem) {
        ret++;
        xcb_screen_next(&iter);
    }
    return ret;
}

void ncxb_init_ncurses(void) {
    window = initscr();
    curs_set(0); // hide cursor
    leaveok(window, true);
    keypad(window, true);
    start_color();
    init_pair(1, COLOR_WHITE & 0xf, COLOR_BLACK * 0x0f);
    //attrset(COLOR_PAIR(1) | (COLOR_WHITE & 0xfffffff0));
    attron(COLOR_PAIR(1));

    getmaxyx(window, height, width);
}

ncxb_output_t ncxb_create_output(xcb_randr_output_t xoutput) {
    ncxb_output_t output;
    int min, max;
    ncxb_range(conn, xoutput, &min, &max);

    output.output = xoutput;
    output.min = min;
    output.max = max;
    output.value = ncxb_get(conn, xoutput);
    return output;
}

ncxb_screen_t ncxb_create_screen(xcb_window_t root) {
        ncxb_screen_t screen;
        xcb_generic_error_t *error;

        xcb_randr_get_screen_resources_cookie_t resources_cookie;
        xcb_randr_get_screen_resources_reply_t *resources_reply;

        resources_cookie = xcb_randr_get_screen_resources(conn, root);
        resources_reply = xcb_randr_get_screen_resources_reply(conn, resources_cookie, &error);
        if(error || !resources_reply) {
            int ec = error ? error->error_code : -1;
            fprintf(stderr, "RANDR Get Screen Resources returned error %d\n", ec);
            exit(-1);
            //return screen; //XXX handle error
        }

        xcb_randr_output_t *outputs = xcb_randr_get_screen_resources_outputs(resources_reply);

        screen.window = root;
        screen.noutputs = resources_reply->num_outputs;
        screen.outputs = malloc(sizeof(ncxb_output_t) * screen.noutputs);

        int i;
        for(i = 0; i < resources_reply->num_outputs; i++) {
            xcb_randr_output_t output = outputs[i];
            screen.outputs[i] = ncxb_create_output(output);
        }

        free(resources_reply);
        return screen;
}

void ncxb_init_xcb(void) {
    conn = xcb_connect(NULL, NULL); // just get the default display
    backlight = ncxb_get_backlight_atom(conn);

    int nscreens = ncxb_count_screens(conn);
    screens = malloc(sizeof(ncxb_screen_t) * nscreens);

    xcb_screen_iterator_t iter;
    iter = xcb_setup_roots_iterator(xcb_get_setup(conn));

    int i = 0;
    while(iter.rem) {
        xcb_screen_t *screen = iter.data;
        screens[i] = ncxb_create_screen(screen->root);
        i++;
        xcb_screen_next(&iter);
    }
}

void ncxb_init(void) {
    signal(SIGINT, ncxb_handle_signal);
    signal(SIGQUIT, ncxb_handle_signal);
    signal(SIGTERM, ncxb_handle_signal);
    // ncurses
    ncxb_init_ncurses();

    // xrandr
    ncxb_init_xcb();
}

void ncxb_exit_ncurses(void) {
    refresh();
    curs_set(1); // show cursor
    leaveok(window, false);
    keypad(window, false);

    endwin();
    window = NULL;
}


void ncxb_exit(void) {
    // ncurses
    ncxb_exit_ncurses();

    //xrandr
    xcb_aux_sync(conn);
}

/*
void ncxb_update_get_values(void) {
    xcb_generic_error_t *error;
    xcb_screen_iterator_t iter;
    iter = xcb_setup_roots_iterator(xcb_get_setup(conn));
    while(iter.rem) {
        xcb_screen_t *screen = iter.data;
        xcb_window_t root = screen->root;

        xcb_randr_get_screen_resources_cookie_t resources_cookie;
        xcb_randr_get_screen_resources_reply_t *resources_reply;

        resources_cookie = xcb_randr_get_screen_resources(conn, root);
        resources_reply = xcb_randr_get_screen_resources_reply(conn, resources_cookie, &error);
        if(error || !resources_reply) {
            int ec = error ? error->error_code : -1;
            fprintf(stderr, "RANDR Get Screen Resources returned error %d\n", ec);
            continue;
        }

        xcb_randr_output_t *outputs = xcb_randr_get_screen_resources_outputs(resources_reply);
        int i;
        for(i = 0; i < resources_reply->num_outputs; i++) {
            xcb_randr_output_t output = outputs[i];
            int min = 0, max = 0, val = 0;
            ncxb_range(conn, output, &min, &max);
            if(!min && !max) continue;
            val = ncxb_get(conn, output);
            values[i] = val * 100 / max;
            xcb_aux_sync(conn);
            //ncxb_set(conn, output, value);
            xcb_flush(conn);
            usleep(200);
        }

        free(resources_reply);
        xcb_screen_next(&iter);
    }
}
*/

void ncxb_update_active_screen(ncxb_screen_t *scr) {
    int key = getch();
    ncxb_output_t *active_out = &scr->outputs[0]; // XXX

    switch(key) {
        case KEY_UP:
            active_out->value += 5;
            if(active_out->value > 100) active_out->value = 100;
            break;
        case KEY_DOWN:
            active_out->value -= 5;
            if(active_out->value < 100) active_out->value = 0;
            break;
        case KEY_LEFT:
            scr->selected--;
            if(scr->selected < 0) scr->selected = 0;
            break;
        case KEY_RIGHT:
            scr->selected++;
            if(scr->selected >= scr->noutputs) scr->selected = scr->noutputs-1;
            break;
        case '\014':
        case 'L':
        case 'l':
            ncxb_clear = true;
            break;
    }
}

void ncxb_update(void) {
    ncxb_update_active_screen(&screens[0]);
}

void draw_value_bar(int x, int y, int h, long barval, char *barnm) {
    // draw bottom of bar
    mvaddch(y, x+2, ACS_LLCORNER);
    mvaddch(y, x+3, ACS_HLINE);
    mvaddch(y, x+4, ACS_HLINE);
    mvaddch(y, x+5, ACS_LRCORNER);

    // draw vertical sides of bar
    int i;
    for(i = 1; i < h; i++) {
        //mvaddstr(y-i, x, "         ");
        mvaddch(y-i, x + 2, ACS_VLINE);
        mvaddch(y-i, x + 5, ACS_VLINE);
    }

    // draw bar contents
    for(i = 1; i < h; i++) {
        int dc = barval > i * 100 / h ? ACS_CKBOARD : ' ';
        mvaddch(y-i, x + 3, dc); //left half of bar
        mvaddch(y-i, x + 4, dc); //right half of bar
    }

    // draw top of bar
    mvaddstr(y - h, x, "         ");
    mvaddch(y - h, x+2, ACS_ULCORNER);
    mvaddch(y - h, x+3, ACS_HLINE);
    mvaddch(y - h, x+4, ACS_HLINE);
    mvaddch(y - h, x+5, ACS_URCORNER);

    // write value of bar below
    char valuestr[128];
    snprintf(valuestr, sizeof(valuestr), "%ld", barval);
    mvaddstr(y+1, x+2, "        ");
    mvaddstr(y+1, x+3 - (barval >= 100), valuestr);

    // write name of bar
    if(barnm) {
        mvaddstr(y+2, x+2, "        ");
        mvaddstr(y+2, x+3 - strlen(barnm)/2, barnm);
    }
}

void draw_frame(int w, int h) {
    int i;

    // top/bottom lines
    for(i = 1; i < w; i++) {
        mvaddch(0, i, ACS_HLINE);
        mvaddch(h - 1, i, ACS_HLINE);
    }

    // side lines
    for(i = 1; i < h; i++) {
        mvaddch(i, 0, ACS_VLINE);
        mvaddch(i, w - 1, ACS_VLINE);
    }

    // corners
    mvaddch(0, 0, ACS_ULCORNER);
    mvaddch(0, w-1, ACS_URCORNER);
    mvaddch(h-1, 0, ACS_LLCORNER);
    mvaddch(h-1, w-1, ACS_LRCORNER);

    char title[128];
    int titlelen;
    sprintf(title, "%s v%s", "NCXBacklight", "0.1");
    titlelen = strlen(title);
    mvaddstr(0, w / 2 - titlelen / 2, title);
}

void draw(void) {
    if(ncxb_clear) {
        clearok(window, true);
    }

    ncxb_screen_t *active = &screens[0];

    int i;
    for(i = 0; i < active->noutputs; i++) {
        draw_value_bar((i+1) * (width / (active->noutputs + 1)) - 2, height - 5, height - 8, active->outputs[i].value, "Main");
    }

    draw_frame(width, height);
    refresh();
}


int main(int argc, char **argv) {
    ncxb_init();

    while(true) {
        draw();
        ncxb_update();
    }

    ncxb_exit();

    return 0;
}

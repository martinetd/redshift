/* gamma-quartz.c -- Quartz (OSX) gamma adjustment
   This file is part of Redshift.

   Redshift is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   Redshift is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with Redshift.  If not, see <http://www.gnu.org/licenses/>.

   Copyright (c) 2014-2017  Jon Lund Steffensen <jonlst@gmail.com>
*/

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <stdio.h>
#include <stdlib.h>

#include <ApplicationServices/ApplicationServices.h>

#ifdef ENABLE_NLS
# include <libintl.h>
# define _(s) gettext(s)
#else
# define _(s) s
#endif

#include "gamma-quartz.h"
#include "colorramp.h"


typedef struct {
	CGDirectDisplayID display;
	uint32_t ramp_size;
	float *saved_ramps;
} quartz_display_state_t;

struct gamma_state {
	quartz_display_state_t *displays;
	uint32_t display_count;
};


static int
quartz_init(gamma_state **state)
{
	*state = malloc(sizeof(gamma_state));
	if (*state == NULL) return -1;

	gamma_state *s = *state;
	s->displays = NULL;

	return 0;
}

static int
quartz_start(gamma_state *state)
{
	CGError error;
	uint32_t display_count;

	/* Get display count */
	error = CGGetOnlineDisplayList(0, NULL, &display_count);
	if (error != kCGErrorSuccess) return -1;

	state->display_count = display_count;

	CGDirectDisplayID* displays =
		malloc(sizeof(CGDirectDisplayID)*display_count);
	if (displays == NULL) {
		perror("malloc");
		return -1;
	}

	/* Get list of displays */
	error = CGGetOnlineDisplayList(display_count, displays,
				       &display_count);
	if (error != kCGErrorSuccess) {
		free(displays);
		return -1;
	}

	/* Allocate list of display state */
	state->displays = malloc(display_count *
				 sizeof(quartz_display_state_t));
	if (state->displays == NULL) {
		perror("malloc");
		free(displays);
		return -1;
	}

	/* Copy display indentifiers to display state */
	for (int i = 0; i < display_count; i++) {
		state->displays[i].display = displays[i];
		state->displays[i].saved_ramps = NULL;
	}

	free(displays);

	/* Save gamma ramps for all displays in display state */
	for (int i = 0; i < display_count; i++) {
		CGDirectDisplayID display = state->displays[i].display;

		uint32_t ramp_size = CGDisplayGammaTableCapacity(display);
		if (ramp_size == 0) {
			fprintf(stderr, _("Gamma ramp size too small: %i\n"),
				ramp_size);
			return -1;
		}

		state->displays[i].ramp_size = ramp_size;

		/* Allocate space for saved ramps */
		state->displays[i].saved_ramps =
			malloc(3 * ramp_size * sizeof(float));
		if (state->displays[i].saved_ramps == NULL) {
			perror("malloc");
			return -1;
		}

		float *gamma_r = &state->displays[i].saved_ramps[0*ramp_size];
		float *gamma_g = &state->displays[i].saved_ramps[1*ramp_size];
		float *gamma_b = &state->displays[i].saved_ramps[2*ramp_size];

		/* Copy the ramps to allocated space */
		uint32_t sample_count;
		error = CGGetDisplayTransferByTable(display, ramp_size,
						    gamma_r, gamma_g, gamma_b,
						    &sample_count);
		if (error != kCGErrorSuccess ||
		    sample_count != ramp_size) {
			fputs(_("Unable to save current gamma ramp.\n"),
			      stderr);
			return -1;
		}
	}

	return 0;
}

static void
quartz_restore(gamma_state *state)
{
	CGDisplayRestoreColorSyncSettings();
}

static void
quartz_free(gamma_state *state)
{
	if (state->displays != NULL) {
		for (int i = 0; i < state->display_count; i++) {
			free(state->displays[i].saved_ramps);
		}
	}
	free(state->displays);
	free(state);
}

static void
quartz_print_help(FILE *f)
{
	fputs(_("Adjust gamma ramps on macOS using Quartz.\n"), f);
	fputs("\n", f);
}

static int
quartz_set_option(gamma_state *state, const char *key, const char *value)
{
	if (strcasecmp(key, "preserve") == 0) {
		fprintf(stderr, _("Parameter `%s` is now always on; "
				  " Use the `%s` command-line option"
				  " to disable.\n"),
			key, "-P");
	} else {
		fprintf(stderr, _("Unknown method parameter: `%s'.\n"), key);
		return -1;
	}

	return 0;
}

static void
quartz_set_temperature_for_display(
	gamma_state *state, int display_index,
	const color_setting_t *setting, int preserve)
{
	CGDirectDisplayID display = state->displays[display_index].display;
	uint32_t ramp_size = state->displays[display_index].ramp_size;

	/* Create new gamma ramps */
	float *gamma_ramps = malloc(3*ramp_size*sizeof(float));
	if (gamma_ramps == NULL) {
		perror("malloc");
		return;
	}

	float *gamma_r = &gamma_ramps[0*ramp_size];
	float *gamma_g = &gamma_ramps[1*ramp_size];
	float *gamma_b = &gamma_ramps[2*ramp_size];

	if (preserve) {
		/* Initialize gamma ramps from saved state */
		memcpy(gamma_ramps, state->displays[display_index].saved_ramps,
		       3*ramp_size*sizeof(float));
	} else {
		/* Initialize gamma ramps to pure state */
		for (int i = 0; i < ramp_size; i++) {
			float value = (double)i/ramp_size;
			gamma_r[i] = value;
			gamma_g[i] = value;
			gamma_b[i] = value;
		}
	}

	colorramp_fill_float(gamma_r, gamma_g, gamma_b, ramp_size,
			     setting);

	CGError error =
		CGSetDisplayTransferByTable(display, ramp_size,
					    gamma_r, gamma_g, gamma_b);
	if (error != kCGErrorSuccess) {
		free(gamma_ramps);
		return;
	}

	free(gamma_ramps);
}

static int
quartz_set_temperature(
	gamma_state *state, const color_setting_t *setting, int preserve)
{
	for (int i = 0; i < state->display_count; i++) {
		quartz_set_temperature_for_display(
			state, i, setting, preserve);
	}

	return 0;
}


const gamma_method_t quartz_gamma_method = {
	"quartz", 1,
	quartz_init,
	quartz_start,
	quartz_free,
	quartz_print_help,
	quartz_set_option,
	quartz_restore,
	quartz_set_temperature
};

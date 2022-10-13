/*
 *
 * Copyright (C) 2021 Maxime Schmitt <maxime.schmitt91@gmail.com>
 *
 * This file is part of Nvtop.
 *
 * Nvtop is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Nvtop is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with nvtop.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "nvtop/interface_options.h"
#include "ini.h"
#include "nvtop/interface_common.h"

#include <assert.h>
#include <errno.h>
#include <libgen.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

char config_file_path[PATH_MAX];
const char config_file_location[] = "nvtop/interface.ini";
const char config_conf_path[] = ".config";

static const char *default_config_path(void) {
  char *xdg_config_dir = getenv("XDG_CONFIG_HOME");
  size_t conf_path_length = 0;
  if (!xdg_config_dir) {
    // XDG config dir not set, default to $HOME/.config
    xdg_config_dir = getenv("HOME");
    conf_path_length = sizeof(config_conf_path);
  }
  size_t xdg_path_length = strlen(xdg_config_dir);
  if (xdg_path_length <
      PATH_MAX - conf_path_length - sizeof(config_file_location)) {
    strcpy(config_file_path, xdg_config_dir);
    config_file_path[xdg_path_length] = '/';
    if (conf_path_length) {
      strcpy(config_file_path + xdg_path_length + 1, config_conf_path);
      config_file_path[xdg_path_length + conf_path_length] = '/';
    }
    strcpy(config_file_path + xdg_path_length + 1 + conf_path_length,
           config_file_location);
    return config_file_path;
  } else {
    return NULL;
  }
}

void alloc_interface_options_internals(char *config_location,
                                       unsigned num_devices,
                                       nvtop_interface_option *options) {
  options->device_information_drawn =
      calloc(num_devices, sizeof(*options->device_information_drawn));
  if (!options->device_information_drawn) {
    perror("Cannot allocate memory: ");
    exit(EXIT_FAILURE);
  }
  options->plot_left_to_right = false;
  options->use_color = true;
  options->encode_decode_hiding_timer = 30.;
  options->temperature_in_fahrenheit = false;
  options->config_file_location = NULL;
  options->sort_processes_by = process_memory;
  options->sort_descending_order = true;
  options->update_interval = 1000;
  options->process_fields_displayed = 0;
  if (config_location) {
    options->config_file_location = malloc(strlen(config_location) + 1);
    if (!options->config_file_location) {
      perror("Cannot allocate memory: ");
      exit(EXIT_FAILURE);
    }
    strcpy(options->config_file_location, config_location);
  } else {
    const char *default_path = default_config_path();
    options->config_file_location = malloc(strlen(default_path) + 1);
    if (!options->config_file_location) {
      perror("Cannot allocate memory: ");
      exit(EXIT_FAILURE);
    }
    strcpy(options->config_file_location, default_path);
  }
}

struct nvtop_option_ini_data {
  unsigned num_devices;
  nvtop_interface_option *options;
};

static const char do_not_modify_notice[] =
    "; Please do not edit this file.\n"
    "; The file is automatically generated and modified by nvtop by pressing "
    "F12.\n"
    "; If you wish to modify an option, use nvtop's setup window (F2) and "
    "follow "
    "up by saving the preference (F12).\n";

static const char general_section[] = "GeneralOption";
static const char general_value_use_color[] = "UseColor";
static const char general_value_update_interval[] = "UpdateInterval";

static const char header_section[] = "HeaderOption";
static const char header_value_use_fahrenheit[] = "UseFahrenheit";
static const char header_value_encode_decode_timer[] = "EncodeHideTimer";

static const char chart_section[] = "ChartOption";
static const char chart_value_reverse[] = "ReverseChart";

static const char process_list_section[] = "ProcessListOption";
static const char process_value_sortby[] = "SortBy";
static const char process_value_display_field[] = "DisplayField";
static const char *process_sortby_vals[process_field_count + 1] = {
    "pId",     "user",   "gpuId",    "type",   "gpuRate", "encRate",
    "decRate", "memory", "cpuUsage", "cpuMem", "cmdline", "none"};
static const char process_value_sort_order[] = "SortOrder";
static const char process_sort_descending[] = "descending";
static const char process_sort_ascending[] = "ascending";

static const char device_section[] = "DeviceDrawOption";
static const char device_shown_value[] = "ShownInfo";
static const char *device_draw_vals[plot_information_count + 1] = {
    "gpuRate",         "gpuMemRate",    "encodeRate", "decodeRate",
    "temperature",     "powerDrawRate", "fanSpeed",   "gpuClockRate",
    "gpuMemClockRate", "none"};

static int nvtop_option_ini_handler(void *user, const char *section,
                                    const char *name, const char *value) {
  struct nvtop_option_ini_data *ini_data = (struct nvtop_option_ini_data *)user;
  // General Options
  if (strcmp(section, general_section) == 0) {
    if (strcmp(name, general_value_use_color) == 0) {
      if (strcmp(value, "true") == 0) {
        ini_data->options->use_color = true;
      }
      if (strcmp(value, "false") == 0) {
        ini_data->options->use_color = false;
      }
    }
    if (strcmp(name, general_value_update_interval) == 0) {
      int update_interval;
      if (sscanf(value, "%d", &update_interval) == 1)
        ini_data->options->update_interval = update_interval;
    }
  }
  // Header Options
  if (strcmp(section, header_section) == 0) {
    if (strcmp(name, header_value_use_fahrenheit) == 0) {
      if (strcmp(value, "true") == 0) {
        ini_data->options->temperature_in_fahrenheit = true;
      }
      if (strcmp(value, "false") == 0) {
        ini_data->options->temperature_in_fahrenheit = false;
      }
    }
    if (strcmp(name, header_value_encode_decode_timer) == 0) {
      double value_double;
      if (sscanf(value, "%le", &value_double) == 1)
        ini_data->options->encode_decode_hiding_timer = value_double;
    }
  }
  // Chart Options
  if (strcmp(section, chart_section) == 0) {
    if (strcmp(name, chart_value_reverse) == 0) {
      if (strcmp(value, "true") == 0) {
        ini_data->options->plot_left_to_right = true;
      }
      if (strcmp(value, "false") == 0) {
        ini_data->options->plot_left_to_right = false;
      }
    }
  }
  // Process List Options
  if (strcmp(section, process_list_section) == 0) {
    if (strcmp(name, process_value_sortby) == 0) {
      for (enum process_field i = process_pid; i < process_field_count; ++i) {
        if (strcmp(value, process_sortby_vals[i]) == 0) {
          ini_data->options->sort_processes_by = i;
        }
      }
    }
    if (strcmp(name, process_value_display_field) == 0) {
      for (enum process_field i = process_pid; i < process_field_count + 1;
           ++i) {
        if (strcmp(value, process_sortby_vals[i]) == 0) {
          ini_data->options->process_fields_displayed =
              process_add_field_to_display(
                  i, ini_data->options->process_fields_displayed);
          ini_data->options->process_fields_displayed =
              process_add_field_to_display(
                  process_field_count,
                  ini_data->options->process_fields_displayed);
        }
      }
    }
    if (strcmp(name, process_value_sort_order) == 0) {
      if (strcmp(value, process_sort_descending) == 0) {
        ini_data->options->sort_descending_order = true;
      }
      if (strcmp(value, process_sort_ascending) == 0) {
        ini_data->options->sort_descending_order = false;
      }
    }
  }
  // Per-Device Sections
  assert(ini_data->num_devices < 1000 && "Not enough room for 1000 devices");
  for (unsigned i = 0; i < ini_data->num_devices && i < 1000; ++i) {
    char gpu_section_name[sizeof(device_section) + 4];
    snprintf(gpu_section_name, sizeof(device_section) + 4, "%s%u",
             device_section, i);
    if (strcmp(section, gpu_section_name) == 0) {
      if (strcmp(name, device_shown_value) == 0) {
        for (enum plot_information j = plot_gpu_rate;
             j < plot_information_count + 1; ++j) {
          if (strcmp(value, device_draw_vals[j]) == 0) {
            ini_data->options->device_information_drawn[i] = plot_add_draw_info(
                j, ini_data->options->device_information_drawn[i]);
            ini_data->options->device_information_drawn[i] = plot_add_draw_info(
                plot_information_count,
                ini_data->options->device_information_drawn[i]);
          }
        }
      }
    }
  }
  return 1;
}

bool load_interface_options_from_config_file(unsigned num_devices,
                                             nvtop_interface_option *options) {
  FILE *option_file = fopen(options->config_file_location, "r");
  if (!option_file)
    return false;
  struct nvtop_option_ini_data ini_data = {num_devices, options};
  int retval = ini_parse_file(option_file, nvtop_option_ini_handler, &ini_data);
  fclose(option_file);
  if (!process_is_field_displayed(options->sort_processes_by,
                                  options->process_fields_displayed)) {
    options->sort_processes_by =
        process_default_sort_by_from(options->process_fields_displayed);
  }
  return retval >= 0;
}

static bool create_config_directory_rec(char *config_directory) {
  for (char *index = config_directory + 1; *index != '\0'; ++index) {
    if (*index == '/') {
      *index = '\0';
      if (mkdir(config_directory,
                S_IRWXU | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH)) {
        if (errno != EEXIST) {
          char *error_str = strerror(errno);
          fprintf(stderr, "Could not create directory \"%s\": %s\n",
                  config_directory, error_str);
          return false;
        }
      }
      *index = '/';
    }
  }
  if (mkdir(config_directory,
            S_IRWXU | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH)) {
    if (errno != EEXIST) {
      char *error_str = strerror(errno);
      fprintf(stderr, "Could not create directory \"%s\": %s\n",
              config_directory, error_str);
      return false;
    }
  }
  return true;
}

static const char *boolean_string(bool value) {
  return value ? "true" : "false";
}

bool save_interface_options_to_config_file(
    unsigned num_devices, const nvtop_interface_option *options) {

  char folder_path[PATH_MAX];
  strcpy(folder_path, options->config_file_location);
  char *config_directory = dirname(folder_path);
  if (!create_config_directory_rec(config_directory))
    return false;

  FILE *config_file = fopen(options->config_file_location, "w");
  if (!config_file) {
    char *error_str = strerror(errno);
    fprintf(stderr, "Could not create config file \"%s\": %s\n",
            options->config_file_location, error_str);
    return false;
  }

  fprintf(config_file, "%s", do_not_modify_notice);
  // General Options
  fprintf(config_file, "[%s]\n", general_section);
  fprintf(config_file, "%s = %s\n", general_value_use_color,
          boolean_string(options->use_color));
  fprintf(config_file, "%s = %d\n", general_value_update_interval,
          options->update_interval);

  // Header Options
  fprintf(config_file, "[%s]\n", header_section);
  fprintf(config_file, "%s = %s\n", header_value_use_fahrenheit,
          boolean_string(options->temperature_in_fahrenheit));
  fprintf(config_file, "%s = %e\n", header_value_encode_decode_timer,
          options->encode_decode_hiding_timer);
  fprintf(config_file, "\n");

  // Chart Options
  fprintf(config_file, "[%s]\n", chart_section);
  fprintf(config_file, "%s = %s\n", chart_value_reverse,
          boolean_string(options->plot_left_to_right));
  fprintf(config_file, "\n");

  // Process Options
  fprintf(config_file, "[%s]\n", process_list_section);
  fprintf(config_file, "%s = %s\n", process_value_sort_order,
          options->sort_descending_order ? process_sort_descending
                                         : process_sort_ascending);
  fprintf(config_file, "%s = %s\n", process_value_sortby,
          process_sortby_vals[options->sort_processes_by]);
  bool display_any_field = false;
  for (enum process_field field = process_pid; field < process_field_count;
       ++field) {
    if (process_is_field_displayed(field, options->process_fields_displayed)) {
      fprintf(config_file, "%s = %s\n", process_value_display_field,
              process_sortby_vals[field]);
      display_any_field = true;
    }
  }
  if (!display_any_field)
    fprintf(config_file, "%s = %s\n", process_value_display_field,
            process_sortby_vals[process_field_count]);
  fprintf(config_file, "\n");

  // Per-Device Sections
  for (unsigned i = 0; i < num_devices; ++i) {
    fprintf(config_file, "[%s%u]\n", device_section, i);
    bool draw_any = false;
    for (enum plot_information j = plot_gpu_rate; j < plot_information_count;
         ++j) {
      if (plot_isset_draw_info(j, options->device_information_drawn[i])) {
        fprintf(config_file, "%s = %s\n", device_shown_value,
                device_draw_vals[j]);
        draw_any = true;
      }
    }
    if (!draw_any)
      fprintf(config_file, "%s = %s\n", device_shown_value,
              device_draw_vals[plot_information_count]);
    fprintf(config_file, "\n");
  }

  fclose(config_file);
  return true;
}

extern inline plot_info_to_draw
plot_add_draw_info(enum plot_information set_info, plot_info_to_draw to_draw);

extern inline plot_info_to_draw
plot_remove_draw_info(enum plot_information set_info,
                      plot_info_to_draw to_draw);

extern inline plot_info_to_draw plot_default_draw_info(void);

extern inline bool plot_isset_draw_info(enum plot_information check_info,
                                        plot_info_to_draw to_draw);

extern inline unsigned plot_count_draw_info(plot_info_to_draw to_draw);

extern inline bool
process_is_field_displayed(enum process_field field,
                           process_field_displayed cols_displayed);

extern inline process_field_displayed
process_remove_field_to_display(enum process_field field,
                                process_field_displayed cols_displayed);

extern inline process_field_displayed
process_add_field_to_display(enum process_field field,
                             process_field_displayed cols_displayed);

extern inline process_field_displayed process_default_displayed_field(void);

extern inline unsigned
process_field_displayed_count(process_field_displayed fields_displayed);

enum process_field
process_default_sort_by_from(process_field_displayed fields_displayed) {
  if (process_is_field_displayed(process_memory, fields_displayed))
    return process_memory;
  if (process_is_field_displayed(process_cpu_mem_usage, fields_displayed))
    return process_cpu_mem_usage;
  if (process_is_field_displayed(process_gpu_rate, fields_displayed))
    return process_gpu_rate;
  if (process_is_field_displayed(process_cpu_usage, fields_displayed))
    return process_cpu_usage;
  if (process_is_field_displayed(process_command, fields_displayed))
    return process_command;
  if (process_is_field_displayed(process_type, fields_displayed))
    return process_type;
  if (process_is_field_displayed(process_enc_rate, fields_displayed))
    return process_enc_rate;
  if (process_is_field_displayed(process_dec_rate, fields_displayed))
    return process_dec_rate;
  if (process_is_field_displayed(process_user, fields_displayed))
    return process_user;
  if (process_is_field_displayed(process_gpu_id, fields_displayed))
    return process_gpu_id;
  if (process_is_field_displayed(process_pid, fields_displayed))
    return process_pid;
  return process_field_count;
}
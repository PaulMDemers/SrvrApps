#ifndef DESKTOOLS_APP_H
#define DESKTOOLS_APP_H
#include <srvros/desk_app.h>
#include <srvros/desk_ui.h>
#include <srvros/desk_dialog.h>
#include <srvros/desk_prompt.h>
#include <srvros/desk_searchbar.h>
#include <srvros/desk_popup.h>
#include <srvros/desk_file_picker.h>
#include <srvros/desk_chooser.h>
#include <srvros/desk_clipboard.h>
#include <srvros/desk_file_job.h>
#include <srvros/desk_preferences.h>
#include <srvros/sys.h>
#include <srvros/audio_abi.h>
enum tool_action { TOOL_NEW=1,TOOL_OPEN,TOOL_SAVE,TOOL_PATH,TOOL_BODY,TOOL_UP,TOOL_REFRESH,TOOL_MKDIR,TOOL_LIST,TOOL_NAME,TOOL_PICK_LOCATION,TOOL_PICK_NAME,TOOL_RENAME,TOOL_DUPLICATE,TOOL_DELETE,TOOL_PROMPT,TOOL_BACK,TOOL_FORWARD,TOOL_HOME,TOOL_VOLUME,TOOL_ROOT,TOOL_FIND,TOOL_FIND_NEXT,TOOL_FIND_PREVIOUS,TOOL_FIND_QUERY,TOOL_FIND_CLOSE,TOOL_INFO,TOOL_COPY_PATH,TOOL_REPLACE,TOOL_REPLACEMENT };
enum { SYSTEM_TABLE=40,SYSTEM_PAUSE,SYSTEM_REFRESH,SYSTEM_TABS,SYSTEM_NETWORK_TABLE,SYSTEM_PROCESSES,SYSTEM_NETWORK,SYSTEM_DIAGNOSTICS,SYSTEM_DEVICES,SYSTEM_DEVICE_TABLE,SYSTEM_SENSORS,SYSTEM_SENSOR_TABLE,SYSTEM_BATTERY_START,SYSTEM_BATTERY_STOP };
enum { SETTINGS_POINTER_SPEED=100,SETTINGS_POINTER_ACCELERATION,SETTINGS_BRIGHTNESS,SETTINGS_DISABLE_WHILE_TYPING,SETTINGS_TAP_CLICK,SETTINGS_TWO_FINGER_TAP,SETTINGS_AUDIO_VOLUME,SETTINGS_AUDIO_SPEAKERS,SETTINGS_AUDIO_HEADPHONES,SETTINGS_AUDIO_TEST,SETTINGS_AUDIO_STOP };
enum { CALC_EVALUATE=70,CALC_CLEAR,CALC_USE,CALC_COPY,CALC_HISTORY,CALC_KEY_FIRST=300 };
#define CALC_HISTORY_LIMIT 16
#define CALC_EXPRESSION_BYTES 256
enum tool_pending { TOOL_PENDING_NONE,TOOL_PENDING_NEW,TOOL_PENDING_OPEN,TOOL_PENDING_CLOSE };
#define TOOL_HISTORY 24
struct tool_location { char path[DESK_FILE_PATH];uint64_t selected;int scroll; };
struct tool {
    int role,quit,worker_ready,modal,pending,closing,body_size,title_dirty,new_window;
    struct desk_app window;
    struct desk_ui ui;
    struct desk_dialog dialog;
    struct desk_popup context;
    struct desk_properties properties;
    struct desk_ui info_ui;
    struct desk_button info_buttons[3];
    int info_mode,info_ready;
    char info_path[DESK_FILE_PATH],info_notice[128];
    struct desk_prompt prompt;
    struct desk_searchbar search;
    int search_open,search_focus;
    int prompt_mode;
    struct desk_file_entry file_selected;
    char file_source[DESK_FILE_PATH],file_select_name[96],file_notice[80];
    struct desk_file_picker picker;
    struct desk_chooser chooser;
    int picker_mode,dialog_separate,quit_application,close_canceled;
    struct desk_button buttons[32];
    struct desk_check checks[13];
    struct desk_slider pointer_speed,brightness,audio_volume;
    struct srv_audio_status audio;
    uint64_t audio_serial,audio_at;
    int audio_pending,audio_requested,audio_wanted_volume;
    unsigned audio_queued_op,audio_queued_value,audio_watch_op;
    int brightness_percent,brightness_pending,brightness_refresh;
    uint64_t brightness_serial,brightness_at;
    struct desk_scrollbar settings_scroll;
    struct desk_preferences preferences;
    unsigned preferences_status;
    int settings_capture;
    struct desk_edit path,body,name;
    struct desk_text_buffer path_text,body_text,name_text;
    struct desk_text_history *body_history;
    int body_history_owned;
    char path_store[2][DESK_FILE_PATH],body_store[2][DESK_FILE_BYTES+1],name_store[2][128];
    struct desk_table table,calc_history;
    struct desk_table_row calc_rows[CALC_HISTORY_LIMIT];
    char calc_expressions[CALC_HISTORY_LIMIT][CALC_EXPRESSION_BYTES],calc_results[CALC_HISTORY_LIMIT][64],calc_result[64];
    uint64_t calc_ids[CALC_HISTORY_LIMIT],calc_next_id;
    int calc_count,calc_valid,calc_just_evaluated;
    struct desk_rect calc_keys;

    struct srv_process_info processes[DESK_TABLE_MAX_ROWS];
    struct desk_table_row table_rows[DESK_TABLE_MAX_ROWS];
    char process_numbers[DESK_TABLE_MAX_ROWS][3][24];
    struct srv_meminfo system_memory;
    struct desk_tabs system_tabs;
    struct desk_table network_table;
    struct srv_net_status_info network;
    struct desk_table_row network_rows[16+SRV_NET_ADAPTER_MAX*4];
    char network_labels[SRV_NET_ADAPTER_MAX*4][32];
    char network_values[16+SRV_NET_ADAPTER_MAX*4][80];
    int network_valid;
    struct desk_table device_table;
    struct desk_table_row device_rows[160];
    char device_values[160][4][64];
    unsigned device_count,device_pci_count,device_usb_count;
    int devices_valid;
    struct srv_battery_status battery;
    struct srv_battery_control battery_control;
    uint64_t battery_refresh_tick;
    char battery_notice[96];
    struct desk_table sensor_table;
    struct desk_table_row sensor_rows[9];
    char sensor_values[9][64];
    struct srv_thermal_status thermal;
    int thermal_valid,thermal_fresh,smc_fresh;
    int process_count,system_paused,system_memory_valid;
    struct desk_list_item items[DESK_FILE_ENTRIES];
    struct desk_file_entry entries[DESK_FILE_ENTRIES];
    char details[DESK_FILE_ENTRIES][32],file_dates[DESK_FILE_ENTRIES][24];
    int file_count;
    struct tool_location locations[TOOL_HISTORY];
    int location_count,location_index,location_pending;
    struct desk_file_job job;
    char status[160],saved_path[DESK_FILE_PATH],pending_path[DESK_FILE_PATH];
    char saved_data[DESK_FILE_BYTES+1],clipboard[DESK_FILE_BYTES+1];
    size_t saved_length;
    struct desk_clipboard_client clip;
    unsigned clip_action,clip_target;
    uint64_t clip_revision;
    size_t clip_caret,clip_anchor;
    uint32_t saved_crc,overwrite_crc;
    uint64_t refresh_tick;
};
void tool_new_window(struct tool *tool);
void tool_calculator_init(struct tool *tool);
void tool_calculator_layout(struct tool *tool);
void tool_calculator_draw(struct tool *tool,struct desk_canvas *canvas);
void tool_calculator_action(struct tool *tool,struct desk_action action);
int tool_calculator_key(struct tool *tool,const struct srv_key_event *key);
void tool_battery_capture_tick(struct tool *tool);
void tool_system_init(struct tool *tool);
void tool_system_layout(struct tool *tool);
void tool_system_refresh(struct tool *tool,int force);
void tool_system_draw(struct tool *tool,struct desk_canvas *canvas);
void tool_system_action(struct tool *tool,struct desk_action action);
void tool_settings_tick(struct tool *t);
void tool_settings_audio(struct tool *t,const struct desk_packet *p);
void tool_settings_brightness(struct tool *t,const struct desk_packet *p);
void tool_settings_init(struct tool *tool);
void tool_settings_layout(struct tool *tool);
void tool_settings_draw(struct tool *tool,struct desk_canvas *canvas);
void tool_settings_action(struct tool *tool,struct desk_action action);
void tool_settings_receive(struct tool *tool,const struct desk_packet *packet);
int tool_settings_pointer(struct tool *tool,struct desk_widget_event event);
void tool_settings_scroll(struct tool *tool,int delta);
void tool_clipboard_action(struct tool *tool,struct desk_action action);
void tool_clipboard_result(struct tool *tool,int result);
int tool_listing_decode(struct tool *tool);
void tool_picker_open(struct tool *tool,int mode);
void tool_picker_result(struct tool *tool,struct desk_picker_result action);
void tool_picker_finish(struct tool *tool,int result);
void tool_picker_cancel(struct tool *tool);
void tool_text_open_path(struct tool *tool,const char *path);
void tool_text_save(struct tool *tool,int choose_path);
void tool_text_save_path(struct tool *tool,const char *path);
void tool_context_open(struct tool *tool,int x,int y,int keyboard);
void tool_context_result(struct tool *tool,struct desk_menu_result result);
int tool_command_action(struct tool *tool,unsigned id);
int tool_command_key(struct tool *tool,const struct srv_key_event *key);
void tool_menu_refresh(struct tool *tool);
void tool_menu_action(struct tool *tool,unsigned id);
void tool_dispatch(struct tool *tool,struct desk_action action);
void tool_status(struct tool *tool,const char *message);
int tool_modified(const struct tool *tool);
void tool_dialog(struct tool *tool,int mode);
void tool_layout(struct tool *tool);
void tool_content_focus(struct tool *tool,int active);
void tool_text_search_open(struct tool *tool);
void tool_text_replace_open(struct tool *tool);
void tool_text_replace(struct tool *tool,int all);
void tool_text_search_close(struct tool *tool);
void tool_text_search_step(struct tool *tool,int backwards,int restart);
void tool_text_search_action(struct tool *tool,struct desk_action action);
#define TOOL_TEXT_HISTORY_BYTES (4u*DESK_FILE_BYTES)
int tool_text_history_prepare(struct tool *tool,struct desk_text_history *reserved);
void tool_text_history_release(struct tool *tool);
void tool_text_init(struct tool *tool,const char *path);
void tool_text_action(struct tool *tool,struct desk_action action);
void tool_text_finish(struct tool *tool,int result);
void tool_text_request(struct tool *tool,int pending);
void tool_text_dialog(struct tool *tool,unsigned id);
void tool_files_info_open(struct tool *tool);
void tool_files_info_close(struct tool *tool);
void tool_files_info_layout(struct tool *tool);
void tool_files_info_draw(struct tool *tool,struct desk_canvas *canvas);
void tool_files_info_action(struct tool *tool,struct desk_action action);
void tool_files_info_key(struct tool *tool,const struct srv_key_event *key);
void tool_files_info_finish(struct tool *tool,int result);
void tool_files_copy_path(struct tool *tool);
void tool_files_prompt(struct tool *tool,struct desk_action action);
void tool_files_dialog(struct tool *tool,unsigned id);
void tool_files_layout(struct tool *tool);
void tool_files_draw(struct tool *tool,struct desk_canvas *canvas);
void tool_files_init(struct tool *tool,const char *path);
void tool_files_action(struct tool *tool,struct desk_action action);
void tool_files_finish(struct tool *tool,int result);
void tool_launch(struct tool *tool,int application,const char *path);
void tool_file_capture_setup(int workspace);
void tool_file_capture_controls_enable(int cancel);
int tool_file_capture_controls_tick(struct tool *);
int tool_file_capture_busy(void);
unsigned tool_file_capture_cycles(void);
void tool_file_capture_begin(struct tool *,uint64_t);
void tool_file_capture_finish(struct tool *,int);
int tool_file_capture_report(void);
#endif

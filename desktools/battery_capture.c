/* Explicit automatic study; ordinary System never drives these actions. */
#include "app.h"
#include <srvros/desk_log.h>
static unsigned phase;
static uint64_t begin,epoch;
void tool_battery_capture_tick(struct tool *t) {
    uint64_t now=srv_ticks();
    if(!begin) {
        begin=now;desk_tabs_select(&t->system_tabs,SYSTEM_SENSORS);
        tool_system_layout(t);desk_ui_layout_changed(&t->ui);tool_system_refresh(t,1);
    }
    if(now-begin>9000 || t->battery_notice[0] || t->battery_control.result<SRV_BATTERY_CONTROL_UNAVAILABLE)goto failed;
    if(!t->battery_control.available)return;
    if(phase==0 || phase==3) {
        epoch=t->battery.epoch;
        tool_system_action(t,(struct desk_action){SYSTEM_BATTERY_START,DESK_WIDGET_ACTIVATED});
        if(t->battery_control.pending!=SRV_BATTERY_START)goto failed;
        phase++;
    } else if(phase==1 || phase==4) {
        if(t->battery.state==SRV_BATTERY_UNAVAILABLE || t->battery.state==SRV_BATTERY_FAULTED)goto failed;
        if(t->battery.state!=SRV_BATTERY_ACTIVE || !(t->battery.flags&SRV_BATTERY_FRESH) || t->battery.generation<2)return;
        if(t->battery.epoch!=epoch+1)goto failed;
        desk_log("battery-ui: sampled cycle=%u epoch=%llu generation=%llu\n",phase==1?1:2,
            (unsigned long long)t->battery.epoch,(unsigned long long)t->battery.generation);
        tool_system_action(t,(struct desk_action){SYSTEM_BATTERY_STOP,DESK_WIDGET_ACTIVATED});
        if(t->battery_control.pending!=SRV_BATTERY_STOP)goto failed;
        phase++;
    } else if(phase==2 || phase==5) {
        if(t->battery_control.pending || t->battery.state!=SRV_BATTERY_STOPPED)return;
        if(t->battery.epoch!=epoch+1 || (t->battery.flags&SRV_BATTERY_FRESH))goto failed;
        desk_log("battery-ui: stopped cycle=%u epoch=%llu\n",phase==2?1:2,(unsigned long long)t->battery.epoch);
        phase++;
        if(phase==6) { desk_log("battery-ui: PASS cycles=2 requests=4 stopped=yes\n");t->quit=1; }
    }
    return;
failed:desk_log("battery-ui: FAIL phase=%u\n",phase);t->window.failed=1;
}

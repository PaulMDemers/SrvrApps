#include <srvros/desk_battery.h>
#include "app.h"
#include <srvros/desk_log.h>
#include <stdio.h>
#include <string.h>
static const struct desk_table_column columns[]={
    {"Process",0,0,1},{"PID",64,1,1},{"Parent",88,1,1},{"Mode",112,0,1},{"Mapped",108,1,1}
};
static int compare(const struct srv_process_info *a,const struct srv_process_info *b,int col) {
    int result=0;
    if(col==0) result=strcmp(a->name,b->name);
    else if(col==3) result=strcmp(a->state,b->state);
    else {
        uint64_t left=col==1?a->pid:col==2?a->parent_pid:a->user_mapped_bytes;
        uint64_t right=col==1?b->pid:col==2?b->parent_pid:b->user_mapped_bytes;
        result=left>right?1:left<right?-1:0;
    }
    if(!result) result=a->pid>b->pid?1:a->pid<b->pid?-1:0;
    return result;
}
static void rows(struct tool *t) {
    int col=t->table.sort_column<0?1:t->table.sort_column;
    for(int i=1;i<t->process_count;i++) {
        struct srv_process_info value=t->processes[i];int j=i;
        while(j>0 && compare(&t->processes[j-1],&value,col)*(t->table.descending?-1:1)>0) {
            t->processes[j]=t->processes[j-1];j--;
        }
        t->processes[j]=value;
    }
    for(int i=0;i<t->process_count;i++) {
        struct srv_process_info *p=&t->processes[i];
        snprintf(t->process_numbers[i][0],24,"%llu",(unsigned long long)p->pid);
        snprintf(t->process_numbers[i][1],24,"%llu",(unsigned long long)p->parent_pid);
        snprintf(t->process_numbers[i][2],24,"%llu KiB",(unsigned long long)(p->user_mapped_bytes/1024));
        t->table_rows[i]=(struct desk_table_row){.id=p->pid,.cells={p->name,t->process_numbers[i][0],t->process_numbers[i][1],p->state,t->process_numbers[i][2]}};
    }
    desk_table_rows(&t->table,t->table_rows,t->process_count);t->window.dirty=1;
}
static void ip_string(char *out,size_t size,uint32_t ip) {
    if(!ip) snprintf(out,size,"Not assigned");
    else snprintf(out,size,"%u.%u.%u.%u",ip>>24,(ip>>16)&255,(ip>>8)&255,ip&255);
}
static const char *network_state(const struct tool *t) {
    if(!t->network_valid) return "Network information unavailable";
    if(!t->network.initialized) return "No supported network adapter";
    if((t->network.link_flags&SRV_NET_LINK_KNOWN) && !(t->network.link_flags&SRV_NET_LINK_UP)) return "Network link disconnected";
    if(!t->network.worker_started) return "Network worker is stopped";
    return t->network.local_ip?"IPv4 address configured":"Waiting for an IPv4 address";
}
static void network_refresh(struct tool *t,int force) {
    int previous_valid=t->network_valid;
    uint64_t previous_link=t->network.link_flags,previous_speed=t->network.link_speed_mbps;
    struct srv_net_status_info n={0};
    t->network_valid=srv_net_status_info(&n)==0 && n.abi_version==SRV_NET_ABI_VERSION && n.struct_size>=sizeof(n);
    t->network=t->network_valid?n:(struct srv_net_status_info){0};
    static const char *labels[]={"IPv4 address","Router","DNS server","Hardware address","Address source",
        "Received frames","Sent frames","TCP connections","TCP listeners","UDP sockets","Pending connections","TCP table drops","Link","Link speed","Duplex"};
    unsigned count=sizeof(labels)/sizeof(*labels);
    for(unsigned i=0;i<count;i++) {
        snprintf(t->network_values[i],sizeof(t->network_values[i]),"—");
        t->network_rows[i]=(struct desk_table_row){.id=i+1,.cells={labels[i],t->network_values[i]}};
    }
    if(t->network_valid && n.initialized) {
        ip_string(t->network_values[0],80,n.local_ip);ip_string(t->network_values[1],80,n.router_ip);ip_string(t->network_values[2],80,n.dns_ip);
        snprintf(t->network_values[3],80,"%02x:%02x:%02x:%02x:%02x:%02x",n.local_mac[0],n.local_mac[1],n.local_mac[2],n.local_mac[3],n.local_mac[4],n.local_mac[5]);
        snprintf(t->network_values[4],80,"%s",n.dhcp_configured?"DHCP":n.local_ip?"Static / default":"Not configured");
        uint64_t numbers[]={n.rx_frames,n.tx_frames,n.tcp_connection_count,n.listener_count,n.udp_socket_count,n.pending_count,n.tcp_table_full_drops};
        for(unsigned i=0;i<7;i++) snprintf(t->network_values[i+5],80,"%llu",(unsigned long long)numbers[i]);
        int known=(n.link_flags&SRV_NET_LINK_KNOWN)!=0,up=(n.link_flags&SRV_NET_LINK_UP)!=0;
        snprintf(t->network_values[12],80,"%s",known?(up?"Connected":"Disconnected"):"Not available");
        if(known && up && n.link_speed_mbps) snprintf(t->network_values[13],80,"%llu Mb/s",(unsigned long long)n.link_speed_mbps);
        snprintf(t->network_values[14],80,"%s",known && up && (n.link_flags&SRV_NET_LINK_DUPLEX_KNOWN)?(n.link_flags&SRV_NET_LINK_FULL_DUPLEX?"Full duplex":"Half duplex"):"—");
    }
    if(t->network_valid) {
        for(unsigned i=0;i<n.adapter_count && i<SRV_NET_ADAPTER_MAX;i++) {
            struct srv_net_adapter_info *a=&n.adapters[i];a->driver[sizeof(a->driver)-1]=0;
            const char *fields[]={"role","driver","MAC","link"};
            unsigned base=count;
            for(unsigned j=0;j<4;j++) {
                char *label=t->network_labels[i*4+j];
                snprintf(label,32,"Adapter %u %s",a->id,fields[j]);
                t->network_rows[count]=(struct desk_table_row){.id=100+i*4+j,.cells={label,t->network_values[count]}};
                count++;
            }
            snprintf(t->network_values[base],80,"%s",a->flags&SRV_NET_ADAPTER_ACTIVE?"Active · net0":"Inactive · no IP configuration");
            snprintf(t->network_values[base+1],80,"%s",a->driver);
            snprintf(t->network_values[base+2],80,"%02x:%02x:%02x:%02x:%02x:%02x",a->mac[0],a->mac[1],a->mac[2],a->mac[3],a->mac[4],a->mac[5]);
            const char *state=!(a->link_flags&SRV_NET_LINK_KNOWN)?"Unknown":a->link_flags&SRV_NET_LINK_UP?"Connected":"Disconnected";
            if((a->link_flags&SRV_NET_LINK_UP) && a->link_speed_mbps)
                snprintf(t->network_values[base+3],80,"%s · %llu Mb/s",state,(unsigned long long)a->link_speed_mbps);
            else snprintf(t->network_values[base+3],80,"%s",state);
        }
    }
    desk_table_rows(&t->network_table,t->network_rows,count);
    if(force || previous_valid!=t->network_valid || previous_link!=t->network.link_flags || previous_speed!=t->network.link_speed_mbps) desk_log("desktools: network-refresh valid=%d initialized=%llu ip=%u rx=%llu tx=%llu paused=%d link=%llu speed=%llu monotonic_us=%llu\n",t->network_valid,
        (unsigned long long)t->network.initialized,t->network.local_ip,(unsigned long long)t->network.rx_frames,(unsigned long long)t->network.tx_frames,t->system_paused,(unsigned long long)t->network.link_flags,(unsigned long long)t->network.link_speed_mbps,(unsigned long long)srv_monotonic_us());
}
static const char *device_name(const struct srv_device_info *d) {
    if(d->bus_kind==SRV_DEVICE_USB) {
        if((d->flags&(SRV_DEVICE_KEYBOARD|SRV_DEVICE_TRACKPAD))==(SRV_DEVICE_KEYBOARD|SRV_DEVICE_TRACKPAD)) return "Keyboard and trackpad";
        if(d->flags&SRV_DEVICE_TRACKPAD) return "Trackpad";
        if(d->flags&SRV_DEVICE_KEYBOARD) return "Keyboard";
        if(d->flags&SRV_DEVICE_POINTER) return "Pointer";
        if(d->flags&SRV_DEVICE_NETWORK) return "USB Ethernet";
        if(d->flags&SRV_DEVICE_STORAGE) return "USB storage";
        if(d->class_code==9) return "USB hub";
        if(d->class_code==3) return "USB input device";
        if(d->class_code==2) return "USB communication";
        if(d->class_code==14) return "USB video";
        if(d->class_code==1) return "USB audio";
        return "USB device";
    }
    if(d->class_code==1) return "Storage controller";
    if(d->class_code==2) return "Network controller";
    if(d->class_code==3) return "Display controller";
    if(d->class_code==4&&d->subclass==3) return "HD audio controller";
    if(d->class_code==4) return "Multimedia controller";
    if(d->class_code==6) return "PCI bridge";
    if(d->class_code==12&&d->subclass==3) return "USB controller";
    if(d->class_code==12&&d->subclass==5) return "SMBus controller";
    return "PCI device";
}
static void devices_refresh(struct tool *t,int force) {
    unsigned count=0;t->devices_valid=1;t->device_pci_count=t->device_usb_count=0;
    for(unsigned kind=SRV_DEVICE_PCI;kind<=SRV_DEVICE_USB;kind++) {
        uint64_t index=0;
        for(unsigned attempts=0;attempts<160;attempts++) {
            struct srv_device_info d;long next=srv_device_list(kind,index,&d);
            if(!next) break;
            if(next<0 || (uint64_t)next<=index || d.version!=SRV_DEVICE_VERSION || d.size!=sizeof(d) || d.bus_kind!=kind || count>=160) {
                t->devices_valid=0;break;
            }
            index=(uint64_t)next;
            char (*values)[64]=t->device_values[count];
            snprintf(values[0],64,"%s",device_name(&d));
            snprintf(values[1],64,"%04x:%04x",d.vendor_id,d.product_id);
            if(kind==SRV_DEVICE_PCI) { snprintf(values[2],64,"%02x:%02x.%u",d.bus,d.device,d.function);t->device_pci_count++; }
            else { snprintf(values[2],64,"USB %u · %u",d.port,d.device);t->device_usb_count++; }
            const char *state="Detected";
            if(kind==SRV_DEVICE_USB) {
                unsigned active=SRV_DEVICE_KEYBOARD|SRV_DEVICE_POINTER|SRV_DEVICE_TRACKPAD|SRV_DEVICE_STORAGE|SRV_DEVICE_HUB|SRV_DEVICE_NETWORK;
                state=(d.flags&SRV_DEVICE_FAILED)?"Device error":(d.flags&active)?"Driver active":
                    (d.flags&SRV_DEVICE_CONFIGURED)?"Configured":"No active driver";
            }
            snprintf(values[3],64,"%s",state);
            t->device_rows[count]=(struct desk_table_row){.id=((uint64_t)kind<<32)|((uint64_t)d.index+1),.cells={values[0],values[1],values[2],values[3]}};
            count++;
        }
    }
    t->device_count=count;desk_table_rows(&t->device_table,t->device_rows,count);
    if(force) desk_log("desktools: devices-refresh valid=%d pci=%u usb=%u\n",t->devices_valid,t->device_pci_count,t->device_usb_count);
}
static void sensor_temperature(char *out,int32_t mc) {
    int64_t value=mc;snprintf(out,64,"%s%lld.%01lld °C",value<0?"-":"",(long long)((value<0?-value:value)/1000),(long long)((value<0?-value:value)%1000/100));
}
static void sensors_refresh(struct tool *t,int force) {
    struct srv_thermal_status s={0};long now=srv_monotonic_us();
    t->thermal_valid=srv_thermal_status(&s)==0 && s.version==SRV_THERMAL_VERSION && s.state<=SRV_THERMAL_ACTIVE;
    t->thermal=s;t->thermal_fresh=t->thermal_valid && s.state==SRV_THERMAL_ACTIVE && s.samples && now>=0 && (uint64_t)now>=s.sampled_us && (uint64_t)now-s.sampled_us<=3000000;
    static const char *names[]={"CPU core (boot CPU)","CPU package","Temperature reference","Thermal limit active","Critical temperature","CPU sample age","Battery and AC","Fan speed","Fan sample age"};
    for(unsigned i=0;i<9;i++) { snprintf(t->sensor_values[i],64,"Not available");t->sensor_rows[i]=(struct desk_table_row){.id=i+1,.cells={names[i],t->sensor_values[i]}}; }
    if(t->thermal_fresh) {
        if(s.flags&SRV_THERMAL_CORE_VALID) sensor_temperature(t->sensor_values[0],s.core_mc);
        if(s.flags&SRV_THERMAL_PACKAGE_VALID) sensor_temperature(t->sensor_values[1],s.package_mc);
        if(s.flags&SRV_THERMAL_TARGET_VALID) {
            sensor_temperature(t->sensor_values[2],s.target_mc);
            snprintf(t->sensor_values[3],64,"%s",s.flags&SRV_THERMAL_THROTTLING?"Yes":"No");
            snprintf(t->sensor_values[4],64,"%s",s.flags&SRV_THERMAL_CRITICAL?"Yes":"No");
        }
    }
    if(t->thermal_valid && s.samples && now>=0 && (uint64_t)now>=s.sampled_us)
        snprintf(t->sensor_values[5],64,"%llu ms%s",(unsigned long long)(((uint64_t)now-s.sampled_us)/1000),t->thermal_fresh?"":" · stale");
    struct desk_battery_view battery_view=desk_battery_view(&t->battery);
    snprintf(t->sensor_values[6],64,"%.63s",battery_view.detail);
    struct srv_smc_status fan={0};t->smc_fresh=0;
    if(!srv_smc_status(&fan) && fan.version==SRV_SMC_VERSION && fan.samples && now>=0 && (uint64_t)now>=fan.sampled_us) {
        uint64_t age=(uint64_t)now-fan.sampled_us;
        int fresh=t->smc_fresh=fan.state==SRV_SMC_ACTIVE && (fan.flags&SRV_SMC_FAN_VALID) && age<=3000000;
        snprintf(t->sensor_values[8],64,"%llu ms%s",(unsigned long long)(age/1000),fresh?"":" · stale");
        if(fresh) {
            if(fan.fan_count==2)snprintf(t->sensor_values[7],64,"L %u.%u / R %u.%u RPM",fan.fan_millirpm/1000,(fan.fan_millirpm%1000)/100,fan.fan1_millirpm/1000,(fan.fan1_millirpm%1000)/100);
            else snprintf(t->sensor_values[7],64,"%u.%u RPM",fan.fan_millirpm/1000,(fan.fan_millirpm%1000)/100);
        }
    }
    desk_table_rows(&t->sensor_table,t->sensor_rows,9);
    if(force) desk_log("desktools: sensors-refresh valid=%d fresh=%d state=%u flags=%u samples=%llu\n",t->thermal_valid,t->thermal_fresh,s.state,s.flags,(unsigned long long)s.samples);
}
static void battery_refresh(struct tool *t,int force) {
    uint64_t now=srv_ticks();if(!force && now-t->battery_refresh_tick<25)return;
    t->battery_refresh_tick=now;
    struct srv_battery_status value={0};struct srv_battery_control control={0};
    if(srv_battery_status(&value))value=(struct srv_battery_status){0};
    if(srv_battery_control(SRV_BATTERY_QUERY,0,&control))control=(struct srv_battery_control){0};
    int changed=value.state!=t->battery.state || value.flags!=t->battery.flags || value.generation!=t->battery.generation ||
        memcmp(&control,&t->battery_control,sizeof(control));
    if(changed && control.available && (control.available!=t->battery_control.available || value.state!=t->battery.state || value.epoch!=t->battery.epoch || (value.generation!=t->battery.generation && value.generation<=2)))
        desk_log("battery-ui: state=%u epoch=%llu generation=%llu pending=%u\n",value.state,(unsigned long long)value.epoch,(unsigned long long)value.generation,control.pending);
    t->battery=value;t->battery_control=control;
    struct desk_battery_view view=desk_battery_view(&value);
    snprintf(t->sensor_values[6],64,"%.63s",view.detail);
    if(changed) { tool_system_layout(t);t->window.dirty=1; }
}
void tool_system_refresh(struct tool *t,int force) {
    battery_refresh(t,force);
    uint64_t now=srv_ticks();if(!force && (t->system_paused || now-t->refresh_tick<100)) return;
    int trace=force || now/1000!=t->refresh_tick/1000;t->refresh_tick=now;
    long index=0;int count=0;struct srv_process_info p;
    while(count<DESK_TABLE_MAX_ROWS && (index=srv_proc_list((uint64_t)index,&p))>0) {
        p.name[sizeof(p.name)-1]=0;p.state[sizeof(p.state)-1]=0;t->processes[count++]=p;
    }
    if(force) desk_log("desktools: system-refresh processes=%d paused=%d\n",count,t->system_paused);
    t->process_count=count;t->system_memory_valid=srv_meminfo(&t->system_memory)==0;rows(t);network_refresh(t,trace);devices_refresh(t,force);sensors_refresh(t,force);
    if(desk_tabs_selected(&t->system_tabs)!=SYSTEM_PROCESSES) snprintf(t->status,sizeof(t->status),"%s · %s",t->system_paused?"Paused":"Updates every second","F5 refreshes");
    else if(index<0) tool_status(t,"Process list unavailable. Press Refresh to try again.");
    else snprintf(t->status,sizeof(t->status),"%d processes%s · F6 selects headings · F5 refreshes",count,t->system_paused?" · Paused":"");
}
void tool_system_init(struct tool *t) {
    desk_tabs_init(&t->system_tabs);
    const struct desk_tab tabs[]={{SYSTEM_PROCESSES,"Processes",0},{SYSTEM_NETWORK,"Network",0},{SYSTEM_DEVICES,"Devices",0},{SYSTEM_SENSORS,"Sensors",0}};
    desk_tabs_items(&t->system_tabs,tabs,4);
    desk_table_init(&t->device_table);
    static const struct desk_table_column device_columns[]={{"Device",0,0,0},{"Vendor:product",132,0,0},{"Location",112,0,0},{"State",160,0,0}};
    desk_table_columns(&t->device_table,device_columns,4);
    desk_ui_add(&t->ui,SYSTEM_DEVICE_TABLE,DESK_CONTROL_TABLE,&t->device_table,NULL);
    desk_table_init(&t->network_table);
    static const struct desk_table_column network_columns[]={{"Property",0,0,0},{"Value",0,0,0}};
    desk_table_columns(&t->network_table,network_columns,2);
    desk_table_init(&t->sensor_table);desk_table_columns(&t->sensor_table,network_columns,2);
    desk_ui_add(&t->ui,SYSTEM_SENSOR_TABLE,DESK_CONTROL_TABLE,&t->sensor_table,NULL);
    desk_ui_add(&t->ui,SYSTEM_TABS,DESK_CONTROL_TABS,&t->system_tabs,NULL);
    desk_ui_add(&t->ui,SYSTEM_NETWORK_TABLE,DESK_CONTROL_TABLE,&t->network_table,NULL);
    desk_table_init(&t->table);desk_table_columns(&t->table,columns,5);desk_table_sort_indicator(&t->table,1,0);
    desk_ui_add(&t->ui,SYSTEM_TABLE,DESK_CONTROL_TABLE,&t->table,NULL);
    desk_ui_add(&t->ui,SYSTEM_PAUSE,DESK_CONTROL_BUTTON,&t->buttons[0],"Pause");
    desk_ui_add(&t->ui,SYSTEM_REFRESH,DESK_CONTROL_BUTTON,&t->buttons[1],"Refresh");
    desk_ui_add(&t->ui,SYSTEM_DIAGNOSTICS,DESK_CONTROL_BUTTON,&t->buttons[2],"Diagnostics");
    desk_ui_add(&t->ui,SYSTEM_BATTERY_START,DESK_CONTROL_BUTTON,&t->buttons[3],"Start monitoring");
    desk_ui_add(&t->ui,SYSTEM_BATTERY_STOP,DESK_CONTROL_BUTTON,&t->buttons[4],"Stop monitoring");
    tool_system_refresh(t,1);desk_log("desktools: system-ready processes=%d total=%llu\n",t->process_count,(unsigned long long)t->system_memory.total_bytes);
}
void tool_system_layout(struct tool *t) {
    int w=t->window.width,h=t->window.height;
    desk_button_configure(&t->buttons[0],(struct desk_rect){w-196,18,80,28},1);
    desk_button_configure(&t->buttons[1],(struct desk_rect){w-104,18,88,28},1);
    desk_button_configure(&t->buttons[2],w>=500?(struct desk_rect){w-324,18,116,28}:(struct desk_rect){0},w>=500);
    desk_tabs_configure(&t->system_tabs,(struct desk_rect){16,56,w-32,32},1);
    int page=desk_tabs_selected(&t->system_tabs),network=page==SYSTEM_NETWORK,devices=page==SYSTEM_DEVICES;
    desk_table_configure(&t->sensor_table,page==SYSTEM_SENSORS?(struct desk_rect){16,132,w-32,h>250?h-250:0}:(struct desk_rect){0},page==SYSTEM_SENSORS);
    struct desk_battery_controls control=desk_battery_controls(&t->battery,&t->battery_control);
    desk_button_configure(&t->buttons[3],page==SYSTEM_SENSORS?(struct desk_rect){16,h-104,164,28}:(struct desk_rect){0},page==SYSTEM_SENSORS && control.start);
    desk_button_configure(&t->buttons[4],page==SYSTEM_SENSORS?(struct desk_rect){192,h-104,164,28}:(struct desk_rect){0},page==SYSTEM_SENSORS && control.stop);
    desk_table_configure(&t->device_table,devices?(struct desk_rect){16,132,w-32,h>206?h-206:0}:(struct desk_rect){0},devices);
    desk_table_configure(&t->table,page!=SYSTEM_PROCESSES?(struct desk_rect){0}:(struct desk_rect){16,158,w-32,h>232?h-232:0},page==SYSTEM_PROCESSES);
    desk_table_configure(&t->network_table,network?(struct desk_rect){16,132,w-32,h>176?h-176:0}:(struct desk_rect){0},network);
}
void tool_system_action(struct tool *t,struct desk_action a) {
    if(a.id==SYSTEM_TABLE && (a.flags&DESK_TABLE_SORT)) {
        rows(t);desk_log("desktools: system-sort column=%d descending=%d\n",t->table.sort_column,t->table.descending);
    }
    if(!(a.flags&DESK_WIDGET_ACTIVATED)) return;
    if(a.id==SYSTEM_PROCESSES || a.id==SYSTEM_NETWORK || a.id==SYSTEM_DEVICES || a.id==SYSTEM_SENSORS) { desk_tabs_select(&t->system_tabs,a.id);a.id=SYSTEM_TABS; }
    if(a.id==SYSTEM_TABS) {
        tool_system_layout(t);desk_ui_layout_changed(&t->ui);tool_system_refresh(t,1);
        desk_log("desktools: system-page=%s\n",desk_tabs_selected(&t->system_tabs)==SYSTEM_NETWORK?"network":desk_tabs_selected(&t->system_tabs)==SYSTEM_DEVICES?"devices":desk_tabs_selected(&t->system_tabs)==SYSTEM_SENSORS?"sensors":"processes");
    }
    if(a.id==SYSTEM_BATTERY_START || a.id==SYSTEM_BATTERY_STOP) {
        struct desk_battery_controls view=desk_battery_controls(&t->battery,&t->battery_control);
        unsigned op=a.id==SYSTEM_BATTERY_START?SRV_BATTERY_START:SRV_BATTERY_STOP;
        if(!(op==SRV_BATTERY_START?view.start:view.stop))return;
        long result=srv_battery_control(op,t->battery.epoch,&t->battery_control);
        snprintf(t->battery_notice,sizeof(t->battery_notice),"%s",result==SRV_BATTERY_CONTROL_STALE?"Battery state changed. Try again.":
            result==SRV_BATTERY_CONTROL_BUSY?"A battery request is already pending.":result?"Battery request was not accepted.":"");
        tool_system_layout(t);t->window.dirty=1;
        desk_log("battery-ui: request=%u result=%ld serial=%llu epoch=%llu\n",op,result,(unsigned long long)t->battery_control.serial,(unsigned long long)t->battery.epoch);
    }
    if(a.id==SYSTEM_DIAGNOSTICS) tool_launch(t,DESK_APP_NETWORK,"");
    if(a.id==SYSTEM_REFRESH) tool_system_refresh(t,1);
    if(a.id==SYSTEM_PAUSE) {
        t->system_paused=!t->system_paused;
        for(unsigned i=0;i<t->ui.count;i++) if(t->ui.controls[i].id==SYSTEM_PAUSE) t->ui.controls[i].label=t->system_paused?"Resume":"Pause";
        tool_system_refresh(t,1);desk_log("desktools: system-paused=%d\n",t->system_paused);
    }
}
void tool_system_draw(struct tool *t,struct desk_canvas *c) {
    const struct desk_theme *theme=&desk_theme_quiet;
    desk_label_draw(c,(struct desk_rect){16,18,c->width-(c->width>=500?352:224),30},theme->heading,"System",theme->text);
    if(desk_tabs_selected(&t->system_tabs)==SYSTEM_NETWORK) {
        desk_label_draw(c,(struct desk_rect){16,98,c->width-32,26},theme->body,network_state(t),theme->text);return;
    }
    if(desk_tabs_selected(&t->system_tabs)==SYSTEM_SENSORS) {
        const char *state=(t->thermal_fresh || t->smc_fresh || desk_battery_view(&t->battery).fresh)?"Sensors · updates every second":t->thermal_valid&&t->thermal.samples?"Sensor readings are stale":"No active sensor provider";
        desk_label_draw(c,(struct desk_rect){16,98,c->width-32,26},theme->body,state,theme->text);
        desk_label_draw(c,(struct desk_rect){16,c->height-64,c->width-32,24},theme->body,t->battery_notice[0]?t->battery_notice:desk_battery_controls(&t->battery,&t->battery_control).message,theme->muted);return;
    }
    char label[128];
    if(desk_tabs_selected(&t->system_tabs)==SYSTEM_DEVICES) {
        snprintf(label,sizeof(label),"%u PCI functions · %u USB devices%s",t->device_pci_count,t->device_usb_count,t->devices_valid?"":" · incomplete inventory");
        desk_label_draw(c,(struct desk_rect){16,98,c->width-32,26},theme->body,label,theme->text);
        desk_label_draw(c,(struct desk_rect){16,c->height-64,c->width-32,24},theme->body,"Detected hardware may still need a driver. Inventory reflects this boot.",theme->muted);return;
    }
    if(t->system_memory_valid) {
        struct srv_meminfo *m=&t->system_memory;
        snprintf(label,sizeof(label),"Memory: %llu MiB used · %llu MiB free · %llu MiB RAM",
            (unsigned long long)(m->used_bytes/1048576),(unsigned long long)(m->free_bytes/1048576),(unsigned long long)(m->total_bytes/1048576));
    } else strcpy(label,"Memory information unavailable");
    desk_label_draw(c,(struct desk_rect){16,98,c->width-32,26},theme->body,label,theme->text);
    struct desk_rect bar={16,134,c->width-32,8};desk_fill(c,bar,theme->border);
    if(t->system_memory_valid && t->system_memory.total_bytes && t->system_memory.used_bytes<=t->system_memory.total_bytes) {
        bar.w=(int)((uint64_t)bar.w*t->system_memory.used_bytes/t->system_memory.total_bytes);desk_fill(c,bar,theme->accent);
    }
    desk_label_draw(c,(struct desk_rect){16,c->height-64,c->width-32,24},theme->body,"Mapped = private user pages; shared surfaces and kernel memory excluded.",theme->muted);
}

#include "frontend_backend.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"

enum { SLOT_COUNT=BRIDGE_DEV_MNGR_MAX_DEVICES };
typedef enum { POWER, LEVEL, MIRED, MIN_MIRED, MAX_MIRED, CONTACT, LUX, OCCUPANCY, HUE, SATURATION, PROPERTY_COUNT } property_t;
static const bridge_dev_mngr_attribute_path_t paths[PROPERTY_COUNT]={
    [POWER]={6,0}, [LEVEL]={8,0}, [MIRED]={0x300,7},
    [MIN_MIRED]={0x300,0x400b}, [MAX_MIRED]={0x300,0x400c},
    [CONTACT]={0x45,0}, [LUX]={0x400,0}, [OCCUPANCY]={0x406,0},
    [HUE]={0x300,0}, [SATURATION]={0x300,1},
};
typedef struct { bool known; uint64_t number; } scalar_t;
typedef struct {
    bridge_dev_mngr_device_info_t device;
    uint32_t generation, revision;
    int64_t next_poll;
    bool pending;
    scalar_t values[PROPERTY_COUNT];
} slot_t;
enum { REQUEST_FREE, REQUEST_PENDING, REQUEST_DONE, REQUEST_COUNT=32 };
typedef struct {
    unsigned state, slot;
    property_t property;
    bool write;
    uint32_t generation, revision, epoch;
    bridge_dev_mngr_handle_t manager;
    char id[BRIDGE_DEV_MNGR_DEVICE_ID_MAX];
    uint16_t endpoint;
    esp_err_t error;
    scalar_t value;
} request_t;
/* Static contexts survive re-init and are never recycled before callback.
 * Repeated detach/reinsert cannot allocate unbounded memory or alias an old
 * callback to a newer request. Only the owner task touches slots/snapshot. */
static request_t requests[REQUEST_COUNT];
static slot_t slots[SLOT_COUNT];
static bridge_dev_mngr_device_info_t catalog[BRIDGE_DEV_MNGR_MAX_DEVICES];
static portMUX_TYPE lock=portMUX_INITIALIZER_UNLOCKED;
static uint32_t epoch, seen_epoch, generation;
static bridge_dev_mngr_handle_t manager;
static bridge_dev_mngr_listener_id_t listener_id;
static bool listener_registered;
static frontend_backend_snapshot_t snapshot;
static uint32_t device_completion[SLOT_COUNT];
static esp_err_t device_error[SLOT_COUNT];
static size_t catalog_count;
static bool catalog_ready, selection_chosen;
static char selected_id[BRIDGE_DEV_MNGR_DEVICE_ID_MAX];

static int find_slot(const char *id)
{
    if(!id || !id[0])return -1;
    for(unsigned i=0;i<SLOT_COUNT;i++)
        if(slots[i].device.active && !strcmp(id,slots[i].device.definition.id))return (int)i;
    return -1;
}
static bool is_light(const char *p)
{
    return !strcmp(p,BRIDGE_DEV_MNGR_PROFILE_ON_OFF_LIGHT)||
        !strcmp(p,BRIDGE_DEV_MNGR_PROFILE_DIMMABLE_LIGHT)||!strcmp(p,BRIDGE_DEV_MNGR_PROFILE_COLOR_LIGHT);
}
static int first_profile(const char *profile)
{
    for(unsigned i=0;i<SLOT_COUNT;i++)
        if(slots[i].device.active&&!strcmp(slots[i].device.definition.profile_id,profile))return (int)i;
    return -1;
}
static int compatibility_slot(frontend_backend_device_t device)
{
    if(device==FRONTEND_DEVICE_LIGHT)return find_slot(selected_id);
    if(device==FRONTEND_DEVICE_PLUG)return first_profile(BRIDGE_DEV_MNGR_PROFILE_OUTLET);
    if(device==FRONTEND_DEVICE_DOOR)return first_profile(BRIDGE_DEV_MNGR_PROFILE_CONTACT_SENSOR);
    return -1;
}

static uint32_t current_epoch(void)
{ portENTER_CRITICAL(&lock);uint32_t value=epoch;portEXIT_CRITICAL(&lock);return value; }
static void changed(bridge_dev_mngr_handle_t m,void *ctx)
{ (void)m;(void)ctx;portENTER_CRITICAL(&lock);++epoch;portEXIT_CRITICAL(&lock); }
static scalar_t scalar(const bridge_dev_mngr_value_t *v)
{
    scalar_t out={0};
    if(v->truncated)return out;
    if(v->type==BRIDGE_DEV_MNGR_VALUE_BOOL){out.known=true;out.number=v->data.boolean;}
    else if(v->type==BRIDGE_DEV_MNGR_VALUE_UNSIGNED){out.known=true;out.number=v->data.unsigned_value;}
    return out;
}
static void completed(bridge_dev_mngr_handle_t m,const bridge_dev_mngr_request_result_t *result,void *ctx)
{
    request_t *r=ctx;
    portENTER_CRITICAL(&lock);
    if(r->state==REQUEST_PENDING){
        r->error=result->error;
        if(m!=r->manager || strcmp(result->device.definition.id,r->id) ||
           result->device.endpoint_id!=r->endpoint)r->error=ESP_ERR_INVALID_STATE;
        /* A successful committed write confirms the submitted value. Some
         * backends omit a value in write completion. Never apply before this. */
        if(!r->write)r->value=r->error==ESP_OK?scalar(&result->value):(scalar_t){0};
        r->state=REQUEST_DONE;
    }
    portEXIT_CRITICAL(&lock);
}
static void feedback(unsigned slot,esp_err_t error)
{
    if(slot<SLOT_COUNT){++device_completion[slot];device_error[slot]=error;}
    ++snapshot.completion_serial;
    snapshot.completion_device=FRONTEND_DEVICE_LIGHT;
    if(slot<SLOT_COUNT){
        const char *profile=slots[slot].device.definition.profile_id;
        if(!strcmp(profile,BRIDGE_DEV_MNGR_PROFILE_OUTLET))snapshot.completion_device=FRONTEND_DEVICE_PLUG;
        else if(!strcmp(profile,BRIDGE_DEV_MNGR_PROFILE_CONTACT_SENSOR))snapshot.completion_device=FRONTEND_DEVICE_DOOR;
    }
    snapshot.completion_error=error;
    snprintf(snapshot.message,sizeof(snapshot.message),"%s",error==ESP_OK?"Updated":esp_err_to_name(error));
}
static void reset_slot(unsigned i,const bridge_dev_mngr_device_info_t *device)
{
    if(slots[i].pending)feedback(i,ESP_ERR_INVALID_STATE);
    memset(&slots[i],0,sizeof(slots[i]));slots[i].generation=++generation;
    if(device)slots[i].device=*device;
}
static void reconcile(void)
{
    size_t count=0;
    uint32_t observed=current_epoch();
    bool invalidated=observed!=seen_epoch;
    seen_epoch=observed;
    esp_err_t error=manager?bridge_dev_mngr_get_devices(manager,catalog,BRIDGE_DEV_MNGR_MAX_DEVICES,&count):ESP_ERR_INVALID_STATE;
    catalog_ready=error==ESP_OK;
    if(!catalog_ready)count=0;
    if(count>BRIDGE_DEV_MNGR_MAX_DEVICES)count=BRIDGE_DEV_MNGR_MAX_DEVICES;
    catalog_count=count;
    /* Slot identities stay stable even when enumeration order changes. */
    for(unsigned s=0;s<SLOT_COUNT;s++){
        const bridge_dev_mngr_device_info_t *candidate=NULL;
        for(size_t i=0;i<count;i++)if(catalog[i].active &&
            !strcmp(catalog[i].definition.id,slots[s].device.definition.id)){candidate=&catalog[i];break;}
        slot_t *slot=&slots[s];
        if(!candidate){if(slot->device.definition.id[0])reset_slot(s,NULL);continue;}
        if(invalidated || candidate->endpoint_id!=slot->device.endpoint_id ||
            candidate->reachable!=slot->device.reachable ||
            strcmp(candidate->definition.profile_id,slot->device.definition.profile_id))reset_slot(s,candidate);
        else slot->device=*candidate;
    }
    for(size_t i=0;i<count;i++){
        if(!catalog[i].active||find_slot(catalog[i].definition.id)>=0)continue;
        for(unsigned s=0;s<SLOT_COUNT;s++)if(!slots[s].device.active){reset_slot(s,&catalog[i]);break;}
    }
    if(!selection_chosen){
        const char *first=NULL;
        for(unsigned s=0;s<SLOT_COUNT;s++)if(slots[s].device.active &&
            is_light(slots[s].device.definition.profile_id) &&
            (!first||strcmp(slots[s].device.definition.id,first)<0))first=slots[s].device.definition.id;
        if(first){snprintf(selected_id,sizeof(selected_id),"%s",first);selection_chosen=true;}
    }
}
static bool supported(const slot_t *slot,property_t p)
{
    const char *profile=slot->device.definition.profile_id;
    if(p==POWER)return !strcmp(profile,BRIDGE_DEV_MNGR_PROFILE_ON_OFF_LIGHT)||!strcmp(profile,BRIDGE_DEV_MNGR_PROFILE_DIMMABLE_LIGHT)||
        !strcmp(profile,BRIDGE_DEV_MNGR_PROFILE_COLOR_LIGHT)||!strcmp(profile,BRIDGE_DEV_MNGR_PROFILE_OUTLET);
    if(p==LEVEL)return !strcmp(profile,BRIDGE_DEV_MNGR_PROFILE_DIMMABLE_LIGHT)||!strcmp(profile,BRIDGE_DEV_MNGR_PROFILE_COLOR_LIGHT);
    if(p==MIRED||p==MIN_MIRED||p==MAX_MIRED||p==HUE||p==SATURATION)return !strcmp(profile,BRIDGE_DEV_MNGR_PROFILE_COLOR_LIGHT);
    if(p==CONTACT)return !strcmp(profile,BRIDGE_DEV_MNGR_PROFILE_CONTACT_SENSOR);
    if(p==LUX)return !strcmp(profile,BRIDGE_DEV_MNGR_PROFILE_ILLUMINANCE_SENSOR);
    return p==OCCUPANCY&&!strcmp(profile,BRIDGE_DEV_MNGR_PROFILE_OCCUPANCY_SENSOR);
}
static esp_err_t submit(unsigned s,property_t p,const bridge_dev_mngr_value_t *value)
{
    slot_t *slot=&slots[s];request_t *r=NULL;
    portENTER_CRITICAL(&lock);
    for(unsigned i=0;i<REQUEST_COUNT;i++){
        if(!value && requests[i].state!=REQUEST_FREE && !requests[i].write && requests[i].slot==s &&
           requests[i].generation==slot->generation && requests[i].property==p){portEXIT_CRITICAL(&lock);return ESP_OK;}
        if(requests[i].state==REQUEST_FREE && !r)r=&requests[i];
    }
    if(r){
        *r=(request_t){.state=REQUEST_PENDING,.slot=s,.property=p,.write=value!=NULL,
            .generation=slot->generation,.revision=slot->revision,.epoch=seen_epoch,.manager=manager,
            .endpoint=slot->device.endpoint_id};
        memcpy(r->id,slot->device.definition.id,sizeof(r->id));
        if(value)r->value=scalar(value);
    }
    portEXIT_CRITICAL(&lock);
    if(!r)return ESP_ERR_NO_MEM;
    bridge_dev_mngr_request_id_t id;
    esp_err_t error=value?bridge_dev_mngr_write_attribute(manager,r->id,&paths[p],value,&id,completed,r):
        bridge_dev_mngr_read_attribute(manager,r->id,&paths[p],&id,completed,r);
    if(error!=ESP_OK){portENTER_CRITICAL(&lock);r->state=REQUEST_FREE;portEXIT_CRITICAL(&lock);}
    return error;
}
void frontend_backend_init(bridge_dev_mngr_handle_t next)
{
    if(manager && listener_registered)bridge_dev_mngr_remove_listener(manager,listener_id);
    listener_registered=false;manager=next;
    portENTER_CRITICAL(&lock);++epoch;seen_epoch=epoch;portEXIT_CRITICAL(&lock);
    memset(&snapshot,0,sizeof(snapshot));
    selected_id[0]='\0';selection_chosen=false;catalog_count=0;catalog_ready=false;
    memset(device_completion,0,sizeof(device_completion));
    memset(device_error,0,sizeof(device_error));
    for(unsigned i=0;i<SLOT_COUNT;i++){memset(&slots[i],0,sizeof(slots[i]));slots[i].generation=++generation;}
    if(manager){
        bridge_dev_mngr_listener_t listener={.device_set_changed=changed};
        esp_err_t error=bridge_dev_mngr_add_listener(manager,&listener,&listener_id);
        listener_registered=error==ESP_OK;
        if(!listener_registered){manager=NULL;feedback(SLOT_COUNT,error);}
    }
}
static bool valid(const scalar_t *v,uint64_t maximum)
{ return v->known && v->number<=maximum; }
static void render_device(unsigned index,frontend_backend_device_state_t *d)
{
    const slot_t *slot=&slots[index];
    memset(d,0,sizeof(*d));
    d->available=slot->device.active;d->reachable=d->available&&slot->device.reachable;d->pending=slot->pending;
    d->completion_serial=device_completion[index];d->completion_error=device_error[index];
    memcpy(d->id,slot->device.definition.id,sizeof(d->id));
    memcpy(d->name,slot->device.definition.name,sizeof(d->name));
    memcpy(d->profile_id,slot->device.definition.profile_id,sizeof(d->profile_id));
    d->device_type=slot->device.definition.device_type;
    d->generation=slot->generation;d->endpoint_id=slot->device.endpoint_id;
    d->supports_power=supported(slot,POWER);d->supports_brightness=supported(slot,LEVEL);
    d->supports_kelvin=supported(slot,MIRED);d->supports_color=supported(slot,HUE);
    if(!d->reachable)return;
    const scalar_t *v=slot->values;
    property_t power=supported(slot,CONTACT)?CONTACT:POWER;
    d->on_known=valid(&v[power],1);d->on=d->on_known&&v[power].number;
    d->brightness_known=valid(&v[LEVEL],254);
    if(d->brightness_known)d->brightness_percent=(uint8_t)((v[LEVEL].number*100+127)/254);
    d->hue_known=valid(&v[HUE],254);if(d->hue_known)d->hue=(uint8_t)v[HUE].number;
    d->saturation_known=valid(&v[SATURATION],254);if(d->saturation_known)d->saturation=(uint8_t)v[SATURATION].number;
    d->kelvin_known=valid(&v[MIRED],65534)&&v[MIRED].number>0;
    if(d->kelvin_known)d->kelvin=(uint32_t)((1000000+v[MIRED].number/2)/v[MIRED].number);
    d->kelvin_bounds_known=valid(&v[MIN_MIRED],65534)&&valid(&v[MAX_MIRED],65534)&&
        v[MIN_MIRED].number>0&&v[MAX_MIRED].number>=v[MIN_MIRED].number;
    if(d->kelvin_bounds_known){d->min_kelvin=(uint32_t)((1000000+v[MAX_MIRED].number-1)/v[MAX_MIRED].number);
        d->max_kelvin=(uint32_t)(1000000/v[MIN_MIRED].number);}
    d->contact_known=valid(&v[CONTACT],1);d->contact_closed=d->contact_known&&v[CONTACT].number;
    d->occupancy_known=valid(&v[OCCUPANCY],255);d->occupied=d->occupancy_known&&(v[OCCUPANCY].number&1);
    d->illuminance_known=valid(&v[LUX],65534);
    if(d->illuminance_known)d->illuminance_lux=v[LUX].number?pow(10.0,((double)v[LUX].number-1)/10000):0;
}
static void render_snapshot(void)
{
    memset(snapshot.devices,0,sizeof(snapshot.devices));
    for(unsigned i=0;i<FRONTEND_DEVICE_COUNT;i++){
        int slot=compatibility_slot((frontend_backend_device_t)i);
        if(slot>=0)render_device((unsigned)slot,&snapshot.devices[i]);
    }
    if(selected_id[0]&&!snapshot.devices[FRONTEND_DEVICE_LIGHT].id[0])
        snprintf(snapshot.devices[FRONTEND_DEVICE_LIGHT].id,sizeof(snapshot.devices[0].id),"%s",selected_id);
    snprintf(snapshot.selected_id,sizeof(snapshot.selected_id),"%s",selected_id);
    snapshot.catalog_ready=catalog_ready;snapshot.catalog_count=0;
    memset(snapshot.catalog,0,sizeof(snapshot.catalog));
    for(size_t i=0;i<catalog_count;i++)if(catalog[i].active){
        int slot=find_slot(catalog[i].definition.id);
        if(slot>=0)render_device((unsigned)slot,&snapshot.catalog[snapshot.catalog_count++]);
    }
    int ldr=first_profile(BRIDGE_DEV_MNGR_PROFILE_ILLUMINANCE_SENSOR);
    snapshot.ldr_endpoint_id = ldr < 0 ? 0 : slots[ldr].device.endpoint_id;
    snapshot.ldr_known=false;snapshot.illuminance_lux=0;snapshot.ldr_percent=0;
    if(ldr>=0){
        const scalar_t *lux=&slots[ldr].values[LUX];
        snapshot.ldr_known=slots[ldr].device.reachable&&valid(lux,65534);
        if(snapshot.ldr_known){
            snapshot.illuminance_lux=lux->number?pow(10.0,((double)lux->number-1)/10000):0;
            double percent=snapshot.illuminance_lux/4;
            snapshot.ldr_percent=percent>=100?100:(uint8_t)(percent+.5);
        }
    }
    int pir=first_profile(BRIDGE_DEV_MNGR_PROFILE_OCCUPANCY_SENSOR);
    snapshot.pir_endpoint_id = pir < 0 ? 0 : slots[pir].device.endpoint_id;
    snapshot.pir_known=false;snapshot.pir=false;
    if(pir>=0){
        const scalar_t *v=&slots[pir].values[OCCUPANCY];
        snapshot.pir_known=slots[pir].device.reachable&&valid(v,255);
        snapshot.pir=snapshot.pir_known&&(v->number&1);
    }
}
void frontend_backend_tick(frontend_backend_snapshot_t *out)
{
    reconcile();int64_t now=esp_timer_get_time();
    for(unsigned i=0;i<REQUEST_COUNT;i++){
        request_t result;bool done=false;
        portENTER_CRITICAL(&lock);
        if(requests[i].state==REQUEST_DONE){result=requests[i];requests[i].state=REQUEST_FREE;done=true;}
        portEXIT_CRITICAL(&lock);
        if(!done)continue;
        slot_t *slot=&slots[result.slot];
        if(result.generation!=slot->generation || result.epoch!=seen_epoch || result.manager!=manager)continue;
        if(result.write){
            slot->pending=false;feedback(result.slot,result.error);slot->next_poll=now;
            if(result.error==ESP_OK)slot->values[result.property]=result.value;
        }else if(result.revision==slot->revision)slot->values[result.property]=result.value;
    }
    for(unsigned s=0;s<SLOT_COUNT;s++){
        slot_t *slot=&slots[s];
        if(!manager||!slot->device.active||!slot->device.reachable||slot->pending||now<slot->next_poll)continue;
        slot->next_poll=now+1000000;
        /* Every catalog endpoint gets its own primary state, never first-type
         * aliases. Detail-only reads stay focused so a large catalog cannot
         * starve later devices behind the bounded callback context pool. */
        bool selected=!strcmp(slot->device.definition.id,selected_id);
        for(property_t p=POWER;p<PROPERTY_COUNT;p++)if(supported(slot,p) &&
            (selected||p==POWER||p==CONTACT||p==LUX||p==OCCUPANCY))submit(s,p,NULL);
    }
    render_snapshot();if(out)*out=snapshot;
}
esp_err_t frontend_backend_select(const char *id)
{
    if(!id||!id[0]||strlen(id)>=sizeof(selected_id))return ESP_ERR_INVALID_ARG;
    int slot=find_slot(id);
    if(slot<0||!catalog_ready||current_epoch()!=seen_epoch)return ESP_ERR_NOT_FOUND;
    bridge_dev_mngr_device_info_t current;
    if(!manager||bridge_dev_mngr_get_device(manager,id,&current)!=ESP_OK||!current.active||
        current.endpoint_id!=slots[slot].device.endpoint_id)return ESP_ERR_NOT_FOUND;
    snprintf(selected_id,sizeof(selected_id),"%s",id);selection_chosen=true;
    slots[slot].next_poll=0;
    return ESP_OK;
}
esp_err_t frontend_backend_command_id(const char *id,frontend_backend_command_t command,int32_t value)
{
    if(!id||!id[0]||strlen(id)>=BRIDGE_DEV_MNGR_DEVICE_ID_MAX)return ESP_ERR_INVALID_ARG;
    int index=find_slot(id);
    if(index<0){feedback(SLOT_COUNT,ESP_ERR_INVALID_STATE);return ESP_ERR_INVALID_STATE;}
    unsigned device=(unsigned)index;
    esp_err_t error=ESP_OK;
    bool needs_bounds=false;
    property_t property=POWER;
    bridge_dev_mngr_value_t v={.type=BRIDGE_DEV_MNGR_VALUE_UNSIGNED};
    slot_t *slot=&slots[device];
    if(command==FRONTEND_COMMAND_POWER){property=POWER;v.type=BRIDGE_DEV_MNGR_VALUE_BOOL;v.data.boolean=value!=0;if(value<0||value>1)error=ESP_ERR_INVALID_ARG;}
    else if(command==FRONTEND_COMMAND_BRIGHTNESS){property=LEVEL;if(value<1||value>100)error=ESP_ERR_INVALID_ARG;else v.data.unsigned_value=(uint32_t)(value*254+50)/100;}
    else if(command==FRONTEND_COMMAND_HUE||command==FRONTEND_COMMAND_SATURATION){
        property=command==FRONTEND_COMMAND_HUE?HUE:SATURATION;
        if(value<0||value>254)error=ESP_ERR_INVALID_ARG;else v.data.unsigned_value=(uint32_t)value;
    }
    else if(command==FRONTEND_COMMAND_KELVIN){
        property=MIRED;const scalar_t *min=&slot->values[MIN_MIRED],*max=&slot->values[MAX_MIRED];
        if(value<=0)error=ESP_ERR_INVALID_ARG;
        else if(!valid(min,65534)||!valid(max,65534)||!min->number||min->number>max->number){error=ESP_ERR_INVALID_STATE;needs_bounds=true;}
        else if( (uint64_t)value<(1000000+max->number-1)/max->number || (uint64_t)value>1000000/min->number)error=ESP_ERR_INVALID_ARG;
        else v.data.unsigned_value=(1000000+(uint32_t)value/2)/(uint32_t)value;
    }else error=ESP_ERR_NOT_SUPPORTED;
    if(error==ESP_OK && !supported(slot,property))error=ESP_ERR_NOT_SUPPORTED;
    bridge_dev_mngr_device_info_t current;
    if(error==ESP_OK && (!manager||current_epoch()!=seen_epoch||slot->pending||!slot->device.active||!slot->device.reachable||
        bridge_dev_mngr_get_device(manager,slot->device.definition.id,&current)!=ESP_OK||!current.active||!current.reachable||
        current.endpoint_id!=slot->device.endpoint_id||strcmp(current.definition.profile_id,slot->device.definition.profile_id)))error=ESP_ERR_INVALID_STATE;
    if(error==ESP_OK){
        ++slot->revision;slot->pending=true;
        error=submit(device,property,&v);
        if(error!=ESP_OK)slot->pending=false;
    }
    if(needs_bounds && supported(slot,MIRED) && manager && current_epoch()==seen_epoch &&
        !slot->pending && slot->device.active && slot->device.reachable &&
        bridge_dev_mngr_get_device(manager,id,&current)==ESP_OK && current.active && current.reachable &&
        current.endpoint_id==slot->device.endpoint_id &&
        !strcmp(current.definition.profile_id,slot->device.definition.profile_id)){
        /* An All/automation job need not steal the selected detail endpoint to
         * discover another lamp's limits. submit deduplicates outstanding reads;
         * the owner can retry after callbacks arrive on a later tick. */
        (void)submit(device,MIN_MIRED,NULL);
        (void)submit(device,MAX_MIRED,NULL);
    }
    if(error!=ESP_OK)feedback(device,error);
    return error;
}
esp_err_t frontend_backend_command(frontend_backend_device_t device,frontend_backend_command_t command,int32_t value)
{
    if((unsigned)device>=FRONTEND_DEVICE_COUNT)return ESP_ERR_INVALID_ARG;
    if(device==FRONTEND_DEVICE_AC||device==FRONTEND_DEVICE_TV){feedback(SLOT_COUNT,ESP_ERR_NOT_SUPPORTED);return ESP_ERR_NOT_SUPPORTED;}
    int slot=compatibility_slot(device);
    if(slot<0){feedback(SLOT_COUNT,ESP_ERR_INVALID_STATE);return ESP_ERR_INVALID_STATE;}
    return frontend_backend_command_id(slots[slot].device.definition.id,command,value);
}

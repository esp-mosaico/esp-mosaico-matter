#include "frontend_jobs.h"
#include "esp_timer.h"
#include <stdio.h>
#include <string.h>

typedef struct {
    char id[BRIDGE_DEV_MNGR_DEVICE_ID_MAX];
    unsigned steps, current;
    bool active, waiting, automatic, power;
    int level, kelvin;
    uint32_t generation, completion;
    int64_t deadline;
} job_t;
static job_t jobs[BRIDGE_DEV_MNGR_MAX_DEVICES];
static uint32_t serial;
static char message[96];

static void report(const char *value)
{ ++serial; snprintf(message,sizeof(message),"%s",value); }

void frontend_jobs_init(void)
{ memset(jobs,0,sizeof(jobs)); serial=0; message[0]=0; }
void frontend_jobs_cancel(const char *id)
{ for(unsigned i=0;i<BRIDGE_DEV_MNGR_MAX_DEVICES;i++)if(jobs[i].active&&!strcmp(jobs[i].id,id))jobs[i].active=false; }
void frontend_jobs_cancel_automatic(void)
{ for(unsigned i=0;i<BRIDGE_DEV_MNGR_MAX_DEVICES;i++)if(jobs[i].automatic)jobs[i].active=false; }
bool frontend_jobs_queue(const frontend_backend_catalog_entry_t *entry,unsigned steps,
                         bool power,int level,int kelvin,bool automatic)
{
    if(!entry||!entry->available||!entry->reachable||!steps)return false;
    frontend_jobs_cancel(entry->id);
    for(unsigned i=0;i<BRIDGE_DEV_MNGR_MAX_DEVICES;i++)if(!jobs[i].active){
        jobs[i]=(job_t){.active=true,.steps=steps,.power=power,.level=level,.kelvin=kelvin,
            .automatic=automatic,.generation=entry->generation,
            .deadline=esp_timer_get_time()+INT64_C(3000000)};
        snprintf(jobs[i].id,sizeof(jobs[i].id),"%s",entry->id);
        report("Sending...");return true;
    }
    report("Device queue is busy");return false;
}

void frontend_jobs_tick(const frontend_backend_snapshot_t *snapshot)
{
    int64_t now=esp_timer_get_time();
    for(unsigned i=0;i<BRIDGE_DEV_MNGR_MAX_DEVICES;i++){
        job_t *job=&jobs[i];if(!job->active)continue;
        const frontend_backend_catalog_entry_t *entry=NULL;
        for(size_t k=0;k<snapshot->catalog_count;k++)if(!strcmp(snapshot->catalog[k].id,job->id)){entry=&snapshot->catalog[k];break;}
        if(!entry||!entry->available||!entry->reachable||entry->generation!=job->generation){
            job->active=false;report("Device removed or offline");continue;
        }
        if(now>job->deadline){job->active=false;report("Device did not respond; try again");continue;}
        if(job->waiting){
            if(entry->completion_serial==job->completion)continue;
            if(entry->completion_error!=ESP_OK){job->active=false;report("Device update failed");continue;}
            job->waiting=false;job->steps&=~job->current;
            job->deadline=now+INT64_C(3000000);
            if(!job->steps){job->active=false;report("Device updated");continue;}
        }
        if(entry->pending)continue;
        frontend_backend_command_t command;
        int value;
        if(job->steps&FRONTEND_JOB_POWER){job->current=FRONTEND_JOB_POWER;command=FRONTEND_COMMAND_POWER;value=job->power;}
        else if(job->steps&FRONTEND_JOB_LEVEL){job->current=FRONTEND_JOB_LEVEL;command=FRONTEND_COMMAND_BRIGHTNESS;value=job->level;}
        else {job->current=FRONTEND_JOB_KELVIN;command=FRONTEND_COMMAND_KELVIN;value=job->kelvin;}
        job->completion=entry->completion_serial;
        esp_err_t error=frontend_backend_command_id(job->id,command,value);
        if(error==ESP_OK){job->waiting=true;job->deadline=now+INT64_C(5000000);}
        else if(error!=ESP_ERR_NO_MEM&&error!=ESP_ERR_INVALID_STATE){job->active=false;report("Device does not support this setting");}
    }
}
uint32_t frontend_jobs_message_serial(void){return serial;}
const char *frontend_jobs_message(void){return message;}

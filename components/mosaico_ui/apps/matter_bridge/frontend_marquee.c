#include "frontend_marquee.h"
#include <stdio.h>
#include <string.h>

/* Advances from the same DejaVuSans.ttf as the actual authored labels,
 * normalized to 1024 units per em. CJK fallback glyphs use a full em. */
static const uint16_t advances[95] = {326,411,471,858,652,973,799,282,400,400,512,858,326,370,326,345,652,652,652,652,652,652,652,652,652,652,345,345,858,858,858,544,1024,701,703,715,789,647,589,794,770,302,302,672,571,884,766,806,618,806,712,650,626,750,701,1013,702,626,702,400,345,400,858,512,512,628,650,563,650,630,361,650,649,285,285,593,285,998,649,627,650,650,421,534,402,649,606,838,606,606,538,652,345,652,858};

static int text_width(const char *text, int size)
{
    unsigned units=0;
    const unsigned char *p=(const unsigned char *)text;
    while(*p){
        unsigned cp=*p++;
        if(cp>=0xc0&&cp<0xe0&&p[0]){cp=(cp&31)<<6;cp|=*p++&63;}
        else if(cp>=0xe0&&cp<0xf0&&p[0]&&p[1]){cp=(cp&15)<<12;cp|=(p[0]&63)<<6;cp|=p[1]&63;p+=2;}
        else if(cp>=0xf0&&cp<0xf8&&p[0]&&p[1]&&p[2]){cp=(cp&7)<<18;cp|=(p[0]&63)<<12;cp|=(p[1]&63)<<6;cp|=p[2]&63;p+=3;}
        units+=cp>=32&&cp<127?advances[cp-32]:cp>=0x2e80?1024:768;
    }
    return (int)((units*(unsigned)size+1023)/1024);
}

int frontend_marquee_offset(frontend_marquee_t *s,const char *text,int frame,int size,int64_t now)
{
    if(!text)text="";
    if(strcmp(s->text,text)){
        snprintf(s->text,sizeof(s->text),"%s",text);
        s->width_px=text_width(s->text,size);
        s->started_us=now;
    }
    int distance=s->width_px-frame;
    if(distance<=0)return 0;
    const int64_t pause=1000000;
    int64_t travel=(int64_t)distance*1000000/32;
    if(travel<1)travel=1;
    int64_t phase=(now-s->started_us)%(2*(travel+pause));
    if(phase<pause)return 0;
    phase-=pause;
    if(phase<travel)return -(int)(phase*distance/travel);
    phase-=travel;
    if(phase<pause)return -distance;
    phase-=pause;
    return -distance+(int)(phase*distance/travel);
}

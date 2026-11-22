#include "waiting_screen.h"
#include "font.h"
#include <stdio.h>
#include <string.h>

enum { BG=0x10151D, PANEL=0x19222E, WHITE=0xF0F4FA, SOFT=0xBAC6D5, MUTED=0x8594A8,
       GREEN=0x64DC9B, YELLOW=0xFFD078, RED=0xFF8585, BLUE=0x7EBBFF };
static void rect(uint8_t *p, int W, int H, unsigned stride, int x, int y, int w, int h, uint32_t color)
{
    for (int j=y; j<y+h && j<H; j++) {
        if (j<0) continue;
        uint32_t *row=(uint32_t *)(p+(size_t)j*stride);
        for (int i=x; i<x+w && i<W; i++) if(i>=0) row[i]=color;
    }
}
static uint32_t cp(const char **p)
{
    const unsigned char *s=(const unsigned char *)*p; uint32_t c=*s++;
    if(c>=0xE0){c=(c&15)<<12|(s[0]&63)<<6|(s[1]&63);s+=2;}
    else if(c>=0xC0){c=(c&31)<<6|(s[0]&63);s++;}
    *p=(const char *)s;return c;
}
static void text(uint8_t *p,int W,int H,unsigned stride,int x,int y,const char *s,uint32_t color,int scale)
{
    while(*s){
        const uint16_t *g=font_glyph(cp(&s));
        for(int row=0;row<FONT_CELL_H;row++)for(int col=0;col<FONT_CELL_W;col++)
            if(g[row]&(1u<<(15-col)))rect(p,W,H,stride,x+col*scale,y+row*scale,scale,scale,color);
        x+=FONT_CELL_W*scale;
    }
}
void waiting_screen_draw(uint8_t *p,int W,int H,unsigned stride,const char *decoder,const char *audio,
                         int gpu,int game_missing,hdmi_warning warning,const char *version)
{
    int sc=W>=1920 && H>=1080 ? 2 : 1;
    /* Two complete language blocks fit at 720p; 1080p uses the same readable scale. */
    if(H<1400) sc=1;
    int width=1120*sc, height=438*sc;
    int left=(W-width)/2, top=(H-height)/2;
    if (left < 16) left = 16;
    if (top < 8) top = 8;
    int panel_w=W-left*2;
    if(panel_w>width)panel_w=width;
    rect(p,W,H,stride,0,0,W,H,BG);
    int hardware=decoder && !strcmp(decoder,"v4l2");
    int software=decoder && !strcmp(decoder,"ffmpeg");
    int off=!decoder || !strcmp(decoder,"none") || !strcmp(decoder,"null");
    uint32_t video_color=hardware?GREEN:software?YELLOW:off?MUTED:RED;
    int audio_off=!audio || !strcmp(audio,"off");
    uint32_t audio_color=audio_off?MUTED:(!hdmi_audio_output_available() || warning==HDMI_WARNING_OUTPUT)?RED:
                          warning==HDMI_WARNING_NONE?GREEN:YELLOW;
    for(int ru=0;ru<2;ru++){
        int y=top+ru*186*sc, x=left+24*sc;
        rect(p,W,H,stride,left,y,panel_w,170*sc,PANEL);
        rect(p,W,H,stride,left,y,4*sc,170*sc,ru?MUTED:BLUE);
        const char *title=ru?(game_missing?"Ожидание игры":"Док готов. Подключите Switch"):
                              (game_missing?"Waiting for your game":"Dock ready. Connect your Switch");
        text(p,W,H,stride,x,y+14*sc,title,WHITE,sc);
        const char *video=ru?(hardware?"Аппаратное декодирование":software?"Программное декодирование":off?"Изображение: декодирование отключено":"Изображение: неизвестный декодер"):
                             (hardware?"Hardware video decoding":software?"Software video decoding":off?"Video: decoding disabled":"Video: unknown decoder");
        text(p,W,H,stride,x,y+52*sc,video,video_color,sc);
        const char *sound=audio_off?(ru?"Звук: отключён":"Audio: disabled"):
            (!hdmi_audio_output_available() && !hdmi_audio_enabled())?(ru?"Звук: аудиовыход недоступен":"Audio: output unavailable"):
            warning==HDMI_WARNING_NONE?(ru?(hdmi_audio_enabled()?"Звук по HDMI | динамики / наушники":"Звук: выбран отдельный аудиовыход"):
                                          (hdmi_audio_enabled()?"HDMI audio supported | speakers / headphones":"Audio: custom output selected")):
            hdmi_audio_message(warning,ru);
        text(p,W,H,stride,x,y+90*sc,sound,audio_color,sc);
        const char *instruction=ru?(game_missing?"Вернитесь в игру. Экран погас? Нажмите питание дважды.":
                                                                 "Запустите игру | Подключите USB | SysDVR в режиме USB"):
                                         (game_missing?"Return to your game. Screen off? Press Power twice.":
                                                       "Start a game | Connect USB | SysDVR USB mode");
        text(p,W,H,stride,x,y+128*sc,instruction,ru?MUTED:SOFT,sc);
    }
    char footer[256];
    snprintf(footer,sizeof footer,"sysdvr-lite %s  |  Decoder: %s  |  %s",version,decoder?decoder:"none",gpu?"GPU conversion":"CPU conversion");
    text(p,W,H,stride,left+24*sc,top+378*sc,footer,MUTED,sc);
    snprintf(footer,sizeof footer,"Audio device: %s",audio?audio:"off");
    text(p,W,H,stride,left+24*sc,top+410*sc,footer,MUTED,sc);
}

// The pixels of ui/pickmodal.h. Split from the list for the reason that header
// gives: the decisions are host-testable and the drawing is not.
#include "pickmodal.h"
#include "board.h"
#include "paint.h"
#include "jpfont.h"
#include <stdio.h>
#include <string.h>

#define PICK_LINE_H 15
#define PICK_TOP    19

// Three digits and a unit, because the column is 40 pixels wide and a person
// choosing a track needs the order of magnitude, not the byte count.
static void size_text(uint32_t bytes, char *out, size_t outsz) {
    if(bytes<1000u)            snprintf(out,outsz,"%uB",(unsigned)bytes);
    else if(bytes<1000u*1000u) snprintf(out,outsz,"%uK",(unsigned)(bytes/1000u));
    else                       snprintf(out,outsz,"%uM",
                                        (unsigned)(bytes/(1000u*1000u)));
}

static void draw_row(pickmodal_t *p, uint16_t *strip, int strip_y, int rows,
                     const pick_row_t *row, int y, bool selected,
                     uint16_t ink, uint16_t dim) {
    if(selected) {
        paint_fill(2,y-2,LCD_W-4,PICK_LINE_H-2,dim);
        paint_ascii(5,y+1,">",ink);
    }
    // A directory is marked by a trailing separator rather than by colour: the
    // row has to say what Enter will do to someone who cannot tell two greys
    // apart, and this screen is where a wrong guess costs the most.
    char name[PICK_NAME_MAX+2];
    size_t n=strlen(row->name);
    if(n>PICK_NAME_MAX) n=PICK_NAME_MAX;
    memcpy(name,row->name,n);
    if(row->is_dir) name[n++]='/';
    name[n]='\0';
    if(jpfont_ready(JPFONT_TEXT))
        jpfont_draw(JPFONT_TEXT,strip,strip_y,rows,14,y-2,name,n,ink);
    else
        paint_ascii(14,y+1,name,ink);
    if(!row->is_dir) {
        char size[8];
        size_text(row->size,size,sizeof size);
        paint_ascii(LCD_W-6-(int)strlen(size)*6,y+1,size,dim);
    }
}

void pickmodal_draw(pickmodal_t *p) {
    p->dirty=false;
    uint16_t *strip=board_strip();
    uint16_t ink=board_rgb(226,234,244), dim=board_rgb(70,92,120),
             rule=board_rgb(28,44,66), accent=board_rgb(120,200,255),
             back=board_rgb(8,13,22), sel=board_rgb(24,48,78);

    for(int strip_y=0;strip_y<LCD_H;strip_y+=STRIP_H) {
        int rows=LCD_H-strip_y<STRIP_H?LCD_H-strip_y:STRIP_H;
        paint_begin(strip,strip_y,rows);
        for(int i=0;i<LCD_W*rows;i++) strip[i]=back;

        paint_ascii(5,3,p->cfg.title,accent);
        paint_fill(0,13,LCD_W,1,rule);

        if(!p->count)
            paint_ascii(14,PICK_TOP+2,p->cfg.empty,dim);
        for(unsigned r=0;r<PICK_ROWS_VISIBLE;r++) {
            unsigned at=p->top+r;
            if(at<p->first||at>=p->first+p->count) continue;
            bool on=at==p->cursor;
            draw_row(p,strip,strip_y,rows,&p->rows[at-p->first],
                     PICK_TOP+(int)r*PICK_LINE_H,on,ink,on?sel:dim);
        }

        paint_fill(0,LCD_H-11,LCD_W,1,rule);
        paint_ascii(5,LCD_H-8,p->count?p->cfg.hint_rows:p->cfg.hint_empty,dim);
        ESP_ERROR_CHECK(board_present(strip_y,rows,strip));
    }
}

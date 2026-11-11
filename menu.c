#include "menu.h"
#include "terminal.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* Bytes of the UTF-8 character at s (lead byte plus continuation bytes). */
static int cp_len(const char *s)
{
    int n = 1;
    while (((unsigned char)s[n] & 0xC0) == 0x80)
        n++;
    return n;
}

static int item_disabled(const char *item)
{
    return strncmp(item, "[disabled]", 10) == 0;
}

/* [disabled] is an internal marker: it controls selection and colour, but
 * should never take space in the menu presented to the user. */
static const char *item_label(const char *item)
{
    if (!item_disabled(item))
        return item;
    item += 10;
    return *item == ' ' ? item + 1 : item;
}

int menu_edit_key(char *buf,size_t n,size_t *cursor,int key)
{
    size_t len=strlen(buf);if(*cursor>len)*cursor=len;
    if(key==KEY_LEFT) {
        if(*cursor){(*cursor)--;while(*cursor&&((unsigned char)buf[*cursor]&0xc0)==0x80)(*cursor)--;}
    } else if(key==KEY_RIGHT) {
        if(*cursor<len){*cursor+=(size_t)cp_len(buf+*cursor);if(*cursor>len)*cursor=len;}
    } else if(key==1)*cursor=0;
    else if(key==5)*cursor=len;
    else if(key==127||key==8) {
        if(*cursor){size_t previous=*cursor-1;while(previous&&((unsigned char)buf[previous]&0xc0)==0x80)previous--;
            memmove(buf+previous,buf+*cursor,len-*cursor+1);*cursor=previous;}
    } else if(key>=32&&key<256) {
        if(len+1<n){memmove(buf+*cursor+1,buf+*cursor,len-*cursor+1);buf[(*cursor)++]=(char)key;}
    } else return 0;
    return 1;
}

static int prompt_prefix_width(const char *text,size_t bytes)
{
    int width=0;
    for(size_t i=0;i<bytes;) {
        int count=cp_len(text+i);char glyph[8];if(count>7)return width;
        memcpy(glyph,text+i,(size_t)count);glyph[count]=0;width+=term_text_width(glyph);i+=(size_t)count;
    }
    return width;
}

void menu_draw_prompt(const Settings *cfg,const char *label,const char *buf,size_t cursor)
{
    int row=term_rows()-1,cols=term_cols(),col=1;
    term_fill(row,0,1,cols,cfg->background);
    for(const char *p=label;*p;) {
        int count=cp_len(p);char glyph[8];if(count>7)break;
        memcpy(glyph,p,(size_t)count);glyph[count]=0;int width=term_text_width(glyph);
        if(col+width>cols/2)break;
        term_text(row,col,glyph,cfg->muted,cfg->background);col+=width;p+=count;
    }
    size_t len=strlen(buf);if(cursor>len)cursor=len;
    int available=cols-col-2;if(available<0)return;
    size_t start=0;int before=prompt_prefix_width(buf,cursor);
    while(start<cursor&&before>available){size_t count=(size_t)cp_len(buf+start);before-=prompt_prefix_width(buf+start,count);start+=count;}
    int pos=0;
    for(size_t i=start;buf[i];) {
        int count=cp_len(buf+i);char glyph[8];if(count>7)break;
        memcpy(glyph,buf+i,(size_t)count);glyph[count]=0;int width=term_text_width(glyph);
        if(pos+width>available+1)break;
        term_text(row,col+pos,glyph,cfg->text,cfg->background);pos+=width;i+=(size_t)count;
    }
    char glyph[8]=" ";
    if(cursor<len){int count=cp_len(buf+cursor);if(count<=7){memcpy(glyph,buf+cursor,(size_t)count);glyph[count]=0;}}
    term_put(row,col+before,glyph,cfg->background,cfg->text);
}

int menu_prompt(const Settings *cfg, DrawFn draw, void *ctx, const char *label,
                char *buf, size_t n)
{
    size_t cursor = strlen(buf);

    for (;;) {
        draw(ctx);

        menu_draw_prompt(cfg,label,buf,cursor);
        term_end_frame();

        int k = term_read_key();
        if (k == KEY_RESIZE)
            continue;
        if (k == KEY_ESC || k == 3 || k == KEY_EOF)
            return 0;
        if (k == '\r' || k == '\n') {
            if (buf[0])
                return 1;
            continue;
        }
        menu_edit_key(buf,n,&cursor,k);
    }
}

int menu_select(const Settings *cfg, DrawFn draw, void *ctx, const char *title,
                char *const *items, int count, int selected)
{
    if (count <= 0)
        return -1;

    int sel = (selected >= 0 && selected < count) ? selected : 0;
    int top = 0;
    Color bg = cfg->background;

    if (item_disabled(items[sel]))
        for (int i = 0; i < count; i++)
            if (!item_disabled(items[i])) { sel = i; break; }

    for (;;) {
        int rows = term_rows(), cols = term_cols();

        int h = count < rows - 6 ? count : rows - 6;
        if (h < 1)
            h = 1;
        int w = term_text_width(title) + 6;
        for (int i = 0; i < count; i++) {
            int tw = term_text_width(item_label(items[i])) + 4;
            if (tw > w)
                w = tw;
        }
        if (w < 18)
            w = 18;
        if (w > cols - 4)
            w = cols - 4;

        if (sel < top)
            top = sel;
        if (sel >= top + h)
            top = sel - h + 1;

        int x0 = (cols - (w + 2)) / 2;
        int y0 = (rows - (h + 2)) / 2;

        draw(ctx);
        term_fill(y0, x0, h + 2, w + 2, bg);

        term_put(y0, x0, "┌", cfg->border, bg);
        term_put(y0, x0 + w + 1, "┐", cfg->border, bg);
        term_put(y0 + h + 1, x0, "└", cfg->border, bg);
        term_put(y0 + h + 1, x0 + w + 1, "┘", cfg->border, bg);
        for (int c = 1; c <= w; c++) {
            term_put(y0, x0 + c, "─", cfg->border, bg);
            term_put(y0 + h + 1, x0 + c, "─", cfg->border, bg);
        }
        for (int r = 1; r <= h; r++) {
            term_put(y0 + r, x0, "│", cfg->border, bg);
            term_put(y0 + r, x0 + w + 1, "│", cfg->border, bg);
        }

        char tbuf[128];
        snprintf(tbuf, sizeof tbuf, " %s ", title);
        term_text(y0, x0 + 2, tbuf, cfg->text, bg);

        for (int i = 0; i < h; i++) {
            int idx = top + i;
            int row = y0 + 1 + i;
            if (idx == sel) {
                term_fill(row, x0 + 1, 1, w, cfg->text);
                term_text(row, x0 + 2, item_label(items[idx]), bg, cfg->text);
            } else {
                term_text(row, x0 + 2, item_label(items[idx]),
                          item_disabled(items[idx]) ? cfg->muted : cfg->text, bg);
            }
        }
        term_end_frame();

        int k = term_read_key();
        if (k == KEY_RESIZE)
            continue;
        if (k == 'j' || k == KEY_DOWN) {
            int next = (sel + 1) % count;
            while (next != sel && item_disabled(items[next])) next = (next + 1) % count;
            if (!item_disabled(items[next])) sel = next;
        } else if (k == 'k' || k == KEY_UP) {
            int prev = (sel + count - 1) % count;
            while (prev != sel && item_disabled(items[prev])) prev = (prev + count - 1) % count;
            if (!item_disabled(items[prev])) sel = prev;
        } else if (k == '\r' || k == '\n') {
            if (!item_disabled(items[sel])) return sel;
        } else if (k == KEY_ESC || k == 'q' || k == 3 || k == KEY_EOF) {
            return -1;
        }
    }
}

/* ---------- stock picker ---------- */

#define PICK_MAX 31             /* longest ticker that can be typed */

static int ticker_char(int k)
{
    return (k >= 'a' && k <= 'z') || (k >= 'A' && k <= 'Z') || (k >= '0' && k <= '9') ||
           k == '.' || k == '-' || k == '_' || k == '^' || k == '=';
}

/* Fills vis with the items that start with text (case does not matter); returns how many. */
static int filter_items(char *const *items, int count, const char *text, int *vis)
{
    size_t tl = strlen(text);
    int n = 0;
    for (int i = 0; i < count; i++)
        if (strncasecmp(items[i], text, tl) == 0)
            vis[n++] = i;
    return n;
}

static int menu_pick_items(const Settings *cfg, DrawFn draw, void *ctx, const char *title,
                           char *const *items, int count, int current,
                           int allow_new, char *typed, size_t typed_n)
{
    int *vis = malloc(sizeof(int) * (size_t)(count + 1));
    if (!vis)
        return -1;

    char buf[PICK_MAX + 1] = "";
    int sel = 0, top = 0, result = -1;
    Color bg = cfg->background;

    int maxw = 0;                               /* the box keeps one width while typing */
    for (int i = 0; i < count; i++) {
        int tw = term_text_width(items[i]);
        if (tw > maxw)
            maxw = tw;
    }
    if (maxw < PICK_MAX / 2)
        maxw = PICK_MAX / 2;

    for (int first = 1;; first = 0) {
        int len = (int)strlen(buf);
        int n = filter_items(items, count, buf, vis);
        int exact = 0;
        for (int i = 0; i < n; i++)
            if (strcasecmp(items[vis[i]], buf) == 0)
                exact = 1;
        int has_new = allow_new && len > 0 && !exact;
        int total = n + (has_new ? 1 : 0);       /* the last entry is the typed name */

        if (first && len == 0) {                 /* start on the stock being shown */
            for (int i = 0; i < n; i++)
                if (vis[i] == current)
                    sel = i;
        }
        if (sel >= total)
            sel = total > 0 ? total - 1 : 0;

        int rows = term_rows(), cols = term_cols();
        int h = total < rows - 9 ? total : rows - 9;
        if (h < 1)
            h = 1;
        if (sel < top)
            top = sel;
        if (sel >= top + h)
            top = sel - h + 1;
        if (top < 0)
            top = 0;

        int w = maxw + 4;
        if (w > cols - 4)
            w = cols - 4;
        int x0 = (cols - (w + 2)) / 2;
        int y0 = (rows - (h + 4)) / 2;

        draw(ctx);
        term_fill(y0, x0, h + 4, w + 2, bg);

        term_put(y0, x0, "┌", cfg->border, bg);
        term_put(y0, x0 + w + 1, "┐", cfg->border, bg);
        term_put(y0 + 2, x0, "├", cfg->border, bg);
        term_put(y0 + 2, x0 + w + 1, "┤", cfg->border, bg);
        term_put(y0 + h + 3, x0, "└", cfg->border, bg);
        term_put(y0 + h + 3, x0 + w + 1, "┘", cfg->border, bg);
        for (int c = 1; c <= w; c++) {
            term_put(y0, x0 + c, "─", cfg->border, bg);
            term_put(y0 + 2, x0 + c, "─", cfg->border, bg);
            term_put(y0 + h + 3, x0 + c, "─", cfg->border, bg);
        }
        for (int r = 1; r <= h + 2; r++) {
            if (r == 2)
                continue;
            term_put(y0 + r, x0, "│", cfg->border, bg);
            term_put(y0 + r, x0 + w + 1, "│", cfg->border, bg);
        }
        char titlebuf[80];
        snprintf(titlebuf, sizeof titlebuf, " %s ", title);
        term_text(y0, x0 + 2, titlebuf, cfg->text, bg);

        int tw = term_text(y0 + 1, x0 + 2, buf, cfg->text, bg);      /* what is typed */
        term_put(y0 + 1, x0 + 2 + tw, " ", bg, cfg->text);           /* cursor */

        if (total == 0) {
            term_text(y0 + 3, x0 + 2, allow_new ? "type a ticker" : "no matching item",
                      cfg->muted, bg);
        } else {
            for (int i = 0; i < h; i++) {
                int idx = top + i;
                int row = y0 + 3 + i;
                const char *name = idx < n ? items[vis[idx]] : buf;
                if (idx == sel) {
                    term_fill(row, x0 + 1, 1, w, cfg->text);
                    term_text(row, x0 + 2, name, bg, cfg->text);
                } else {
                    term_text(row, x0 + 2, name, cfg->text, bg);
                }
            }
        }
        term_end_frame();

        int k = term_read_key();
        if (k == KEY_RESIZE)
            continue;
        if (k == KEY_UP) {
            if (total > 0)
                sel = (sel + total - 1) % total;
        } else if (k == KEY_DOWN) {
            if (total > 0)
                sel = (sel + 1) % total;
        } else if (k == '\r' || k == '\n') {
            if (total == 0)
                continue;
            if (sel < n) {
                result = vis[sel];
            } else if (allow_new) {
                snprintf(typed, typed_n, "%s", buf);
                result = PICK_NEW;
            }
            break;
        } else if (k == KEY_ESC || k == 3 || k == KEY_EOF) {
            break;
        } else if (k == 127 || k == 8) {
            if (len > 0) {
                buf[len - 1] = '\0';
                sel = top = 0;
            }
        } else if (k < 256 && ticker_char(k) && len < PICK_MAX) {
            buf[len] = (char)toupper(k);
            buf[len + 1] = '\0';
            sel = top = 0;
        }
    }

    free(vis);
    return result;
}

int menu_pick_stock(const Settings *cfg, DrawFn draw, void *ctx, char *const *items,
                    int count, int current, char *typed, size_t typed_n)
{
    return menu_pick_items(cfg, draw, ctx, "Stock", items, count, current, 1, typed, typed_n);
}

int menu_pick_existing(const Settings *cfg, DrawFn draw, void *ctx, const char *title,
                       char *const *items, int count, int current)
{
    return menu_pick_items(cfg, draw, ctx, title, items, count, current, 0, NULL, 0);
}

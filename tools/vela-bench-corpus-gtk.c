// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later
#include <gtk/gtk.h>
#include <gdk/wayland/gdkwayland.h>
#include <fontconfig/fontconfig.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "vela-bench-corpus-content.h"

static GtkWidget *window;
static char *font_file,*font_hash,*font_family,*content_hash,*previous;
static unsigned sequence;
static char *quoted(const char *value)
{
    if(!value)return g_strdup("\"\"");
    GString *s=g_string_new("\"");
    for(const unsigned char *p=(const unsigned char *)value;*p;++p) {
        if(*p=='"' || *p=='\\')g_string_append_c(s,'\\');
        if(*p<32)g_string_append_printf(s,"\\u%04x",*p);else g_string_append_c(s,*p);
    }
    g_string_append_c(s,'"');return g_string_free(s,FALSE);
}
static char *base(void)
{
    char *file=quoted(font_file),*family=quoted(font_family);
    char *value=g_strdup_printf("\"schema\":1,\"method\":\"%s\",\"content_sha256\":\"%s\",\"toolkit\":\"GTK\","
        "\"toolkit_version\":\"%u.%u.%u\",\"pango_version\":\"%s\",\"font_requested\":\"%s\",\"font_size_px\":16,"
        "\"font_resolved\":%s,\"font_file\":%s,\"font_sha256\":\"%s\",\"cases\":[\"gtk-wayland\"]",
        CORPUS_METHOD,content_hash,gtk_get_major_version(),gtk_get_minor_version(),gtk_get_micro_version(),
        pango_version_string(),CORPUS_FONT,family,file,font_hash?font_hash:"");
    g_free(file);g_free(family);return value;
}
static void describe_font(void)
{
    content_hash=g_compute_checksum_for_string(G_CHECKSUM_SHA256,CORPUS_CONTENT,-1);
    FcPattern *pattern=FcPatternCreate();FcPatternAddString(pattern,FC_FAMILY,(const FcChar8 *)CORPUS_FONT);
    FcPatternAddInteger(pattern,FC_WEIGHT,FC_WEIGHT_REGULAR);FcConfigSubstitute(NULL,pattern,FcMatchPattern);FcDefaultSubstitute(pattern);
    FcResult result;FcPattern *match=FcFontMatch(NULL,pattern,&result);FcChar8 *file=NULL,*family=NULL;
    if(match && FcPatternGetString(match,FC_FILE,0,&file)==FcResultMatch
             && FcPatternGetString(match,FC_FAMILY,0,&family)==FcResultMatch) {
        gchar *bytes=NULL;gsize length=0;
        if(g_file_get_contents((const char *)file,&bytes,&length,NULL) && length<=32*1024*1024) {
            font_file=g_strdup((const char *)file);font_family=g_strdup((const char *)family);
            font_hash=g_compute_checksum_for_data(G_CHECKSUM_SHA256,(const guchar *)bytes,length);
        }
        g_free(bytes);
    }
    if(match)FcPatternDestroy(match);
    FcPatternDestroy(pattern);
}
static void draw(GtkDrawingArea *area,cairo_t *cr,int width,int height,gpointer data)
{
    (void)data;
    for(int corner=0;corner<4;++corner) {
        unsigned char rgb[3];corpus_color(1,corner,rgb);
        cairo_set_source_rgb(cr,rgb[0]/255.,rgb[1]/255.,rgb[2]/255.);
        cairo_rectangle(cr,corner%2?width-24:8,corner/2?height-24:8,16,16);cairo_fill(cr);
    }
    GdkSurface *surface=gtk_native_get_surface(gtk_widget_get_native(GTK_WIDGET(area)));
    if(!surface || !gtk_widget_get_mapped(GTK_WIDGET(area)))return;
    char scale[G_ASCII_DTOSTR_BUF_SIZE];g_ascii_dtostr(scale,sizeof scale,gdk_surface_get_scale(surface));
    char *description=base();
    char *identity=g_strdup_printf("%s,\"event\":\"corpus-state\",\"case\":\"gtk-wayland\",\"protocol\":\"wayland\","
        "\"backend\":\"wayland\",\"uid\":%u,\"pid\":%u,\"canvas_width\":%d,\"canvas_height\":%d,"
        "\"window_width\":%d,\"window_height\":%d,\"reported_scale\":%s,\"integer_scale\":%d,\"maximized\":%s",
        description,(unsigned)getuid(),(unsigned)getpid(),width,height,gtk_widget_get_width(window),gtk_widget_get_height(window),
        scale,gdk_surface_get_scale_factor(surface),gtk_window_is_maximized(GTK_WINDOW(window))?"true":"false");
    if(g_strcmp0(identity,previous)) {
        printf("{%s,\"sequence\":%u,\"monotonic_ns\":%lld}\n",identity,++sequence,(long long)g_get_monotonic_time()*1000);
        fflush(stdout);g_free(previous);previous=g_strdup(identity);
    }
    g_free(identity);g_free(description);
}
static gboolean finish(gpointer data) { g_application_quit(G_APPLICATION(data));return G_SOURCE_REMOVE; }
static void activate(GtkApplication *app,gpointer data)
{
    (void)data;
    if(!GDK_IS_WAYLAND_DISPLAY(gdk_display_get_default())) {g_application_quit(G_APPLICATION(app));return;}
    GtkCssProvider *css=gtk_css_provider_new();
    gtk_css_provider_load_from_string(css,"* {font-family: 'DejaVu Sans';font-size:16px;font-weight:normal;}");
    gtk_style_context_add_provider_for_display(gdk_display_get_default(),GTK_STYLE_PROVIDER(css),GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(css);
    window=gtk_application_window_new(app);gtk_window_set_title(GTK_WINDOW(window),"Vela toolkit corpus — GTK Wayland");
    gtk_window_set_default_size(GTK_WINDOW(window),CORPUS_WIDTH,CORPUS_HEIGHT);
    GtkWidget *overlay=gtk_overlay_new();gtk_window_set_child(GTK_WINDOW(window),overlay);
    GtkWidget *box=gtk_box_new(GTK_ORIENTATION_VERTICAL,6);
    gtk_widget_set_margin_start(box,32);gtk_widget_set_margin_end(box,32);gtk_widget_set_margin_top(box,32);gtk_widget_set_margin_bottom(box,32);
    gtk_overlay_set_child(GTK_OVERLAY(overlay),box);
    gtk_box_append(GTK_BOX(box),gtk_label_new("Local scaling corpus"));
    GtkWidget *row=gtk_box_new(GTK_ORIENTATION_HORIZONTAL,6);gtk_box_append(GTK_BOX(box),row);
    GtkWidget *entry=gtk_entry_new();gtk_editable_set_text(GTK_EDITABLE(entry),"0123456789 AaBb CcDd");gtk_editable_set_editable(GTK_EDITABLE(entry),FALSE);
    gtk_editable_set_width_chars(GTK_EDITABLE(entry),12);gtk_widget_set_hexpand(entry,TRUE);gtk_box_append(GTK_BOX(row),entry);
    GtkWidget *check=gtk_check_button_new_with_label("Enabled");gtk_check_button_set_active(GTK_CHECK_BUTTON(check),TRUE);gtk_box_append(GTK_BOX(row),check);
    const char *cells[5][3]={{"Name","Scale","State"},{"Alpha","100%","Ready"},{"Beta","125%","Local"},{"Gamma","150%","Fixed"},{"Delta","200%","Offline"}};
    GtkWidget *grid=gtk_grid_new();gtk_grid_set_column_homogeneous(GTK_GRID(grid),TRUE);gtk_grid_set_row_spacing(GTK_GRID(grid),3);
    for(int y=0;y<5;++y)for(int x=0;x<3;++x)gtk_grid_attach(GTK_GRID(grid),gtk_label_new(cells[y][x]),x,y,1,1);
    gtk_widget_set_vexpand(grid,TRUE);gtk_box_append(GTK_BOX(box),grid);
    row=gtk_box_new(GTK_ORIENTATION_HORIZONTAL,6);gtk_box_append(GTK_BOX(box),row);
    gtk_box_append(GTK_BOX(row),gtk_button_new_with_label("Open"));gtk_box_append(GTK_BOX(row),gtk_button_new_with_label("Save"));
    GtkWidget *area=gtk_drawing_area_new();gtk_widget_set_halign(area,GTK_ALIGN_FILL);gtk_widget_set_valign(area,GTK_ALIGN_FILL);
    gtk_widget_set_can_target(area,FALSE);gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(area),draw,NULL,NULL);
    gtk_overlay_add_overlay(GTK_OVERLAY(overlay),area);gtk_window_present(GTK_WINDOW(window));
    g_timeout_add_seconds(55,finish,app);
}
int main(int argc,char **argv)
{
    describe_font();
    if(argc==2 && !strcmp(argv[1],"--describe")) {char *value=base();printf("{%s}\n",value);g_free(value);return 0;}
    if(getuid()==0 || geteuid()==0 || argc!=3 || strcmp(argv[1],"--case") || strcmp(argv[2],"gtk-wayland"))return 1;
    if(g_strcmp0(font_family,CORPUS_FONT) || !font_hash) {
        puts("{\"schema\":1,\"event\":\"corpus-unsupported\",\"case\":\"gtk-wayland\",\"reason\":\"fixed-font-unavailable\"}");return 77;
    }
    g_setenv("GDK_BACKEND","wayland",TRUE);
    GtkApplication *app=gtk_application_new("org.vela.BenchmarkCorpusGTK",G_APPLICATION_NON_UNIQUE);
    g_signal_connect(app,"activate",G_CALLBACK(activate),NULL);
    char *arguments[]={argv[0],NULL};const int result=g_application_run(G_APPLICATION(app),1,arguments);
    g_object_unref(app);return result;
}

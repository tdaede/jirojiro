#include <gtk/gtk.h>
#include <daala/codec.h>
#include <daala/daaladec.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "draw.h"
#include "internal.h"
#include "jirojiro.h"
 
 extern const int        OD_VERT_D[];
/*The vector offsets in the X direction for each motion comepnsation block
   vertex from the upper-left.*/
# define OD_VERT_DX (OD_VERT_D+1)
/*The vector offsets in the Y direction for each motion compensation block
   vertex from the upper-left.*/
# define OD_VERT_DY (OD_VERT_D+0)
extern const int *const OD_VERT_SETUP_DX[4][4];
extern const int *const OD_VERT_SETUP_DY[4][4];

static cairo_surface_t *surface = NULL;
GtkWidget *coordinates;
GtkWidget *flags_label;
ogg_page page;
ogg_sync_state oy;
ogg_stream_state os;
daala_info di;
daala_comment dc;
daala_setup_info *dsi;
daala_dec_ctx *dctx;
od_img img;

double scale_factor = 1.0;

FILE* input;

jiro_ctx j;

static void
step_frame (GtkWidget *widget,
             gpointer   data) {
  
}

static cairo_surface_t* draw(od_img *img) {
  unsigned char *y_row;
  unsigned char *u_row;
  unsigned char *v_row;
  unsigned char *xrgb;
  unsigned char *rgb;
  int w;
  int h;
  int x;
  int y;
  int i;
  int u_hdec;
  int u_vdec;
  int v_hdec;
  int v_vdec;
  cairo_surface_t *cs;
  unsigned char * pixels;

  w=img->width;
  h=img->height;
  u_hdec=img->planes[1].xdec;
  u_vdec=img->planes[1].ydec;
  v_hdec=img->planes[2].xdec;
  v_vdec=img->planes[2].ydec;
  y_row=img->planes[0].data;
  u_row=img->planes[1].data;
  v_row=img->planes[2].data;
  
  pixels = (unsigned char *)malloc(sizeof(*pixels)*4*w*h);

  /* convert the YUV image into our xRGB pixels buffer (Cairo requires
     unpacked 32 bit RGB with one unused byte).  This code works for
     420, 422 and 444 */
  xrgb=pixels;
  for(y=0;y<h;y++){
    for(x=0;x<w;x++){
      int r;
      int g;
      int b;
      r=OD_DIV_ROUND(2916394880000LL*y_row[x]+
                     4490222169144LL*v_row[x>>v_hdec]-
                     621410755730432LL, 9745792000LL);
      g=OD_DIV_ROUND(2916394880000LL*y_row[x]-
                     534117096223LL*u_row[x>>u_hdec]-
                     1334761232047LL*v_row[x>>v_hdec]+
                     192554107938560LL,9745792000LL);
      b=OD_DIV_ROUND(2916394880000LL*y_row[x]+
                     5290866304968LL*u_row[x>>u_hdec]-
                     723893205115904LL, 9745792000LL);
      xrgb[4*x+0]=OD_CLAMPI(0,b,65535)>>8;
      xrgb[4*x+1]=OD_CLAMPI(0,g,65535)>>8;
      xrgb[4*x+2]=OD_CLAMPI(0,r,65535)>>8;
      xrgb[4*x+3]=255;
    }
    y_row+=img->planes[0].ystride;
    u_row+=img->planes[1].ystride&-((y&1)|!u_vdec);
    v_row+=img->planes[2].ystride&-((y&1)|!v_vdec);
    xrgb+=4*w;
  }

  /* hand pixels to Cairo */
  cs=cairo_image_surface_create_for_data(pixels,CAIRO_FORMAT_ARGB32,w,h,w*4);
  if(cairo_surface_status(cs)!=CAIRO_STATUS_SUCCESS){
    cairo_surface_destroy(cs);
    return NULL;
  }
  return cs;
}

  GtkWidget *da;


static gboolean
draw_cb (GtkWidget *widget,
         cairo_t   *cr,
         gpointer   data)
{
  cairo_scale(cr, scale_factor, scale_factor);
  /* draw video */
  cairo_surface_t* cs = draw(&img);
  cairo_set_source_surface (cr, cs, 0, 0);
  cairo_paint (cr);
  
  /* draw blocks */
  int nhsb = (di.pic_width + (OD_BSIZE_MAX - 1) & ~(OD_BSIZE_MAX - 1)) >> OD_LOG_BSIZE0 + OD_NBSIZES - 1;
  int nvsb = (di.pic_height + (OD_BSIZE_MAX - 1) & ~(OD_BSIZE_MAX - 1)) >> OD_LOG_BSIZE0 + OD_NBSIZES - 1;
  int sbx;
  int sby;
  
  for (sby = 0; sby < nvsb; sby++) {
    for (sbx = 0; sbx < nhsb; sbx++) {
      draw_block_recursive(cr, &j, sbx*8, sby*8, 3);
    }
  }
  
  draw_mvs(cr, &j);
  
  return FALSE;
}

int read_page() {
  while (ogg_sync_pageout(&oy, &page) != 1) {
    char *buffer = ogg_sync_buffer(&oy, 4096);
    if (buffer == NULL) {
      return FALSE;
    }
    int bytes = fread(buffer, 1, 4096, input);
    // End of file
    if (bytes == 0) {
      return FALSE;
    }
    if (ogg_sync_wrote(&oy, bytes) != 0) {
      return FALSE;
    }
  }
  return TRUE;
}

int read_packet(ogg_packet *packet) {
  while (ogg_stream_packetout(&os, packet) != 1) {
    if (!read_page()) {
      return FALSE;
    }
    if (ogg_stream_pagein(&os, &page) != 0) {
      return TRUE;
    }
  }
  return TRUE;
}

static gboolean key_press_cb (GtkWidget *widget, GdkEventKey *event, gpointer user_data) {
  ogg_packet packet;
  if (read_packet(&packet)) {
    if (daala_decode_packet_in(dctx, &img, &packet) != 0) {
      printf("Daala decode fail!\n");
      return -1;
    }
  }
  gtk_widget_queue_draw(da);
  while (gtk_events_pending()) {
    gtk_main_iteration_do(FALSE);
  }
  return FALSE;
}

static const char* band_desc[] = {
  "Coded",
  "Skipped",
  "NoRef",
  "Zeroed"
};

static gboolean pointer_motion_cb (GtkWidget *widget, GdkEventMotion *event, gpointer user_data) {
  char block_text[100];
  char flags_text[1000];
  int flags_text_i = 0;
  int bx = event->x / 4 / scale_factor;
  int by = event->y / 4 / scale_factor;
  int nhsb = (di.pic_width + (OD_BSIZE_MAX - 1) & ~(OD_BSIZE_MAX - 1)) >> OD_LOG_BSIZE0 + OD_NBSIZES - 1;
  int nvsb = (di.pic_height + (OD_BSIZE_MAX - 1) & ~(OD_BSIZE_MAX - 1)) >> OD_LOG_BSIZE0 + OD_NBSIZES - 1;
  if ((bx < nhsb*8) && (by < nvsb*8)) {
    int n = OD_BLOCK_SIZE4x4(j.bsize, j.bstride, bx, by);
    if (n <= 3) {
      int selected_bx = (bx >> n) << n;
      int selected_by = (by >> n) << n;
      int i;
      snprintf(block_text, 100, "Block: (%d, %d) Size: %d", selected_bx, selected_by, n);
      gtk_label_set_text(GTK_LABEL(coordinates), block_text);
      int max_band = n * 3;
      for (i = 0; i <= max_band; i++) {
        int band_flags = (j.flags[selected_by * j.fstride + selected_bx]>>(2*i)) & 0x03;
        flags_text_i += sprintf(flags_text + flags_text_i, "Band %d: %s\n", i, band_desc[band_flags]);
      }
      gtk_label_set_text(GTK_LABEL(flags_label), flags_text);
    }
  }
  return FALSE;
}

int
main (int   argc,
      char *argv[])
{
  GtkWidget *window;
  GtkWidget *headerbar;
  GtkWidget *topbox;
  GtkWidget *sidebar;
  int done = FALSE;

  gtk_init (&argc, &argv);
  if (argc < 2) {
    printf("Usage: %s *.ogv\n",argv[0]);
    return -1;
  }
  
  ogg_sync_init(&oy);
  daala_info_init(&di);
  daala_comment_init(&dc);
  input = fopen(argv[1],"rb");
  while (!done && read_page()) {
    int serial = ogg_page_serialno(&page);
    if (ogg_page_bos(&page)) {
      if (ogg_stream_init(&os, serial) != 0) {
        printf("Could not initialize stream!\n");
        return -1;
      }
    }
    if (ogg_stream_pagein(&os, &page) != 0) {
      return -1;
    }
    ogg_packet packet;
    while (!done && ogg_stream_packetout(&os, &packet) != 0) {
      int ret = daala_decode_header_in(&di, &dc, &dsi, &packet);
      if (ret < 0) {
        if (memcmp(packet.packet, "fishead", packet.bytes)) {
          fprintf(stderr, "Ogg Skeleton streams not supported\n");
        }
        printf("Failed to decode Daala header!\n");
        return -1;
      }
      if (ret == 0) {
        done = TRUE;
        dctx = daala_decode_alloc(&di, dsi);
        if (dctx == NULL) {
          printf("Failed to allocate Daala decoder!\n");
          return -1;
        }
      }
    }
  }
  
  int nhsb = (di.pic_width + (OD_BSIZE_MAX - 1) & ~(OD_BSIZE_MAX - 1)) >> OD_LOG_BSIZE0 + OD_NBSIZES - 1;
  int nvsb = (di.pic_height + (OD_BSIZE_MAX - 1) & ~(OD_BSIZE_MAX - 1)) >> OD_LOG_BSIZE0 + OD_NBSIZES - 1;
  j.bsize = (unsigned char *)malloc(sizeof(*j.bsize)*nhsb*4*nvsb*4);
  j.bstride = nhsb*4;
  j.flags = (unsigned int *)malloc(sizeof(*j.flags)*nhsb*8*nvsb*8);
  j.fstride = nhsb*8;
  daala_decode_ctl(dctx, OD_DECCTL_SET_BSIZE_BUFFER, j.bsize, sizeof(*j.bsize)*nhsb*4*nvsb*4);
  daala_decode_ctl(dctx, OD_DECCTL_SET_FLAGS_BUFFER, j.flags, sizeof(*j.flags)*nhsb*8*nvsb*8);
  int frame_width = (di.pic_width + (OD_SUPERBLOCK_SIZE - 1)) &
   ~(OD_SUPERBLOCK_SIZE - 1);
  int frame_height = (di.pic_height + (OD_SUPERBLOCK_SIZE - 1)) &
   ~(OD_SUPERBLOCK_SIZE - 1);
  int nhmbs = frame_width >> 4;
  int nvmbs = frame_height >> 4;
  j.nhmvbs = frame_width >> OD_LOG_MVBSIZE_MIN;
  j.nvmvbs = frame_height >> OD_LOG_MVBSIZE_MIN;
  j.mv_grid = (od_mv_grid_pt*)malloc(sizeof(od_mv_grid_pt)*(j.nhmvbs+1)*(j.nvmvbs+1));
  j.mv_stride = j.nhmvbs+1;
  int ret = daala_decode_ctl(dctx, OD_DECCTL_SET_MV_BUFFER, j.mv_grid, sizeof(od_mv_grid_pt)*(j.nhmvbs+1)*(j.nvmvbs+1));
  if (ret) {
    if (ret == OD_EINVAL) {
      printf("Allocated the wrong MV buffer size.\n");
    }
    printf("Daala build doesn't support MV buffer reads!\n");
    return -1;
  }
  /* create a new window, and set its title */
  window = gtk_window_new (GTK_WINDOW_TOPLEVEL);
  gtk_window_set_title (GTK_WINDOW (window), "JiroJiro - Daala Visualization Tool");
  gtk_window_set_icon_from_file(GTK_WINDOW(window), "hyperoats.jpg", NULL);
  g_signal_connect (window, "destroy", G_CALLBACK (gtk_main_quit), NULL);
  
  topbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
  gtk_container_add(GTK_CONTAINER(window), topbox);

  da = gtk_drawing_area_new ();
  gtk_widget_set_size_request (da, di.pic_width*scale_factor, di.pic_height*scale_factor);
  gtk_widget_add_events(da, GDK_POINTER_MOTION_MASK);
  gtk_container_add(GTK_CONTAINER(topbox), da);
  
  sidebar = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
  gtk_widget_set_size_request(sidebar, 200, 200);
  gtk_container_add(GTK_CONTAINER(topbox), sidebar);
  gtk_widget_show(sidebar);
  
  coordinates = gtk_label_new("Block:");
  gtk_container_add(GTK_CONTAINER(sidebar), coordinates);
  
  flags_label = gtk_label_new("Flags:");
  gtk_container_add(GTK_CONTAINER(sidebar), flags_label);
  gtk_widget_show(flags_label);
  
  gtk_widget_show (da);
  gtk_widget_show(topbox);
  gtk_widget_show(coordinates);
  gtk_widget_show (window);
    /* Signals used to handle the backing surface */
  g_signal_connect (da, "draw",
                    G_CALLBACK (draw_cb), NULL);
  g_signal_connect (window, "key_press_event", G_CALLBACK (key_press_cb), NULL);
  g_signal_connect (da, "motion-notify-event", G_CALLBACK(pointer_motion_cb), NULL);

  gtk_main ();

  return 0;
}

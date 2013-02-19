/**

File: main.cpp
Copyright 2012 Test Equipment Plus
Author: Justin Crooks
E-Mail: justin@testequipmentplus.com
Description: test GUI file for CMySignalHound Class
			which encapsulates the functionality of the Signal Hound

Revision History:
23 JAN 2012		Justin Crooks		Created
03 FEB 2012		Justin Crooks		Added thread priority to improve service interval for USB

Note: REQUIRES LOW LATENCY LINUX KERNEL OR SIMILAR.  The normal kernel is too slow.
*/

#include <stdlib.h>
#include <gtk/gtk.h>
#include <pthread.h>
#include <math.h>
#include <unistd.h>
#include <string.h>
#include "MySignalHound.h"
#include "Settings.h"
#include "AudioStream.h"
#include "MeasRcvr.h"
#include <ftdi.h>

#include <glib.h>
// ************************* Global variables -- these could easily be in a class or structure to manage your user interface ************

static pthread_attr_t tattr;
static pthread_t sweepThread; //A thread for sweeping, separate from the User Interface thread

static GdkPixmap *pixmap = NULL;            //The actual image for the GtkDrawingArea
static GtkDrawingArea *graticule = NULL;    //The graticule & trace data
static GtkWidget *button = NULL;            //For adding buttons

static GtkWidget *textfreq = NULL;          //Text widgets for displaying settings
static GtkWidget *textRBW = NULL;
static GtkWidget *textspan = NULL;
static GtkWidget *textreflvl = NULL;
static GtkWidget *textdebug = NULL;         //For displaying debug info
static char debugstr[32];

static GtkTextBuffer * fbuf = NULL;         //Text buffers for the above widgets
static GtkTextBuffer * sbuf = NULL;
static GtkTextBuffer * rbuf = NULL;
static GtkTextBuffer * bbuf = NULL;
static GtkTextBuffer * dbuf = NULL;

static GtkWidget *win = NULL;               //Widget for the overall window
static GtkLayout *layout = NULL;
static GdkRectangle r;

static bool g_bHasTrace = false;            //Don't plot until we have valid data
static bool g_bDirty = false;               //Set flag when the trace is "dirty", e.g. needs updated
static bool g_bSettingsChanged = true;      //Set flag when "settings" changed and SetupForSweep is required

static double gCenterFreq = 915.0e6;        //Current sweep state
static double gSpanFreq = 20.0e6;
static double gRefLevel = 0.0;

static CMySignalHound * pMyHound;              //The Signal Hound object

static void UpdateStrings();                //To update text fields

// ************************* Functions **********************************8
static gint
configure_event (GtkWidget *widget, GdkEventConfigure *event) // Configure the drawing area
{
  if (pixmap)
    gdk_pixmap_unref(pixmap);

  pixmap = gdk_pixmap_new(widget->window,
                          widget->allocation.width,
                          widget->allocation.height,
                          -1);
  gdk_draw_rectangle (pixmap,
                      widget->style->white_gc,
                      TRUE,
                      0, 0,
                      widget->allocation.width,
                      widget->allocation.height);

  return TRUE;
}

static gint
expose_event (GtkWidget *widget, GdkEventExpose *event) // Configure the display refresh event
{

  gdk_draw_pixmap(widget->window,
                  widget->style->fg_gc[GTK_WIDGET_STATE (widget)],
                  pixmap,
                  event->area.x, event->area.y,
                  event->area.x, event->area.y,
                  event->area.width, event->area.height);

  return FALSE;
}

int GetXVal(int idx) //For a 500x400 graph, get the x value of a point in the sweep
{
    return (idx * 500) / pMyHound->m_traceSize;
}

int GetYVal(double ampl) //For a 500x400 graph, get the y value of a point in the sweep
{
    double dBampl = 10.0 * log10(ampl);
    int retval = -3.9 * (dBampl - pMyHound->m_settings.m_refLevel);//100 dB is full scale
    if(retval>390) retval=390;
    if(retval<0) retval=0;
    return retval;
}

static void DrawAgain () // Refresh the trace and queue a re-draw
{
    int i;
    gdk_draw_rectangle(pixmap, win->style->white_gc, TRUE, 0,0,502,392);

    for(i=0; i<=10; i++) //Draw graticule
    {
        gdk_draw_line(pixmap,win->style->black_gc,0,i*39,500,i*39);
        gdk_draw_line(pixmap,win->style->black_gc,i*50,0,i*50,390);
    }
    if(g_bHasTrace)
    {
        int lastx=GetXVal(0);
        int lasty=GetYVal(pMyHound->pDataMax[0]);
        for(i=1; i<pMyHound->m_traceSize; i++)
        {
            int thisx = GetXVal(i);
            int thisy = GetYVal(pMyHound->pDataMax[i]);
            gdk_draw_line(pixmap,win->style->black_gc, lastx,lasty,thisx,thisy); //Draw line from last point to this point
            lastx=thisx; lasty=thisy;
        }
    }
    gtk_widget_queue_draw((GtkWidget *)graticule);  //Inform application that a redraw is required
 }

static void * ThreadFunc(void * pv) //Thread for Signal Hound setup and sweeping
{
       sched_param param;
       param.sched_priority = 51;
       pthread_attr_setschedparam(&tattr,&param);
       pthread_setschedparam(pthread_self(),SCHED_RR,&param);
	usleep(10000);
     if(g_bSettingsChanged) //If settings have cahnged since last sweep...
    {
       pMyHound->m_settings.m_refLevel = gRefLevel;
       pMyHound->SetCenterAndSpan(gCenterFreq,gSpanFreq);
       pMyHound->SetupForSweep();
       g_bSettingsChanged = false;
    }
    usleep(10000);
    int errorcode = pMyHound->DoSweep();  //Do the sweep
    usleep(10000);
 
    int peakidx=0;
    for(int i=1; i<pMyHound->m_traceSize; i++)
    {
        if(pMyHound->pDataMax[i] > pMyHound->pDataMax[peakidx]) peakidx = i;//Find peak
    }
    sprintf(debugstr,"pk %d, %.1f dBm",peakidx,mW2dBm(pMyHound->pDataMax[peakidx])); //Display peak index and amplitude
    if(errorcode)
        sprintf(debugstr,"error %d",errorcode);

    g_bHasTrace = true; //Indicate that there is valid data to display
    g_bDirty = true;    //Indicate the disply needs to be updated
    return NULL;
 }
static int g_waittimer=0;
static gboolean TimerFunc() //Periodically check to see if sweep has completed
{
    if(g_waittimer>0)
    {
        g_waittimer--;
         if(g_waittimer==0) pthread_create(&sweepThread, &tattr,ThreadFunc,NULL);
        return true;
    }
   if(g_bDirty) //If trace data has updated, process the trace
   {
        pthread_join(sweepThread,NULL);
	usleep(10000);
        DrawAgain();
 	usleep(10000);
        UpdateStrings();
 	usleep(10000);
        g_bDirty = false;
        g_waittimer = 2;
    }

    return true;
}

static void UpdateStrings() //Update teh text strings
{
    char mystr[32];
    sprintf(mystr,"%.3f MHz",gCenterFreq * 1.0e-6);
    gtk_text_buffer_set_text(fbuf,mystr,-1);

    sprintf(mystr,"%.3f MHz",gSpanFreq * 1.0e-6);
    if(gSpanFreq<1.0e6)
        sprintf(mystr,"%.3f KHz",gSpanFreq * 1.0e-3);
    gtk_text_buffer_set_text(sbuf,mystr,-1);

    sprintf(mystr,"%.1f dBm",gRefLevel);
    gtk_text_buffer_set_text(rbuf,mystr,-1);

    double myRBW = GetRBWFromIndex(pMyHound->m_settings.m_RBWSetpoint);
    sprintf(mystr,"RBW %.1f KHz", myRBW * 1.0e-3);
    if(myRBW<1.0e3)
        sprintf(mystr,"RBW %.1f Hz",myRBW);
    gtk_text_buffer_set_text(bbuf,mystr,-1);

    gtk_text_buffer_set_text(dbuf,debugstr,-1);

}

static void freqplus (GtkWidget *wid, GtkWidget *win)
{
    gCenterFreq += gSpanFreq * 0.5;
    g_bSettingsChanged = true;
    UpdateStrings();
    gtk_widget_queue_draw(win);

}

static void freqminus (GtkWidget *wid, GtkWidget *win)
{
    gCenterFreq -= gSpanFreq * 0.5;
    g_bSettingsChanged = true;
    UpdateStrings();
    gtk_widget_queue_draw(win);
}

static void refplus (GtkWidget *wid, GtkWidget *win)
{
    gRefLevel += 10.0;
    g_bSettingsChanged = true;
    UpdateStrings();
    gtk_widget_queue_draw(win);
}

static void refminus (GtkWidget *wid, GtkWidget *win)
{
    gRefLevel -= 10.0;
    g_bSettingsChanged = true;
    UpdateStrings();
    gtk_widget_queue_draw(win);
}

double SnapToSpan(double span) //Preserve 1, 2, 5 sequence
{
    double retval = span;
    if(span==2.5e9) retval=2.0e9;
    if(span==2.5e8) retval=2.0e8;
    if(span==2.5e7) retval=2.0e7;
    if(span==2.5e6) retval=2.0e6;
    if(span==2.5e5) retval=2.0e5;
    if(span==2.5e4) retval=2.0e4;
    if(span==2.5e3) retval=2.0e3;
    if(span==2.5e2) retval=2.0e2;

    if(span==4.0e2) retval=5.0e2;
    if(span==4.0e3) retval=5.0e3;
    if(span==4.0e4) retval=5.0e4;
    if(span==4.0e5) retval=5.0e5;
    if(span==4.0e6) retval=5.0e6;
    if(span==4.0e7) retval=5.0e7;
    if(span==4.0e8) retval=5.0e8;

    g_bSettingsChanged = true;
    return retval;

}

static void spanplus (GtkWidget *wid, GtkWidget *win)
{
    gSpanFreq = SnapToSpan(gSpanFreq * 2.0);
    UpdateStrings();
   gtk_widget_queue_draw(win);
}

static void spanminus (GtkWidget *wid, GtkWidget *win)
{
    gSpanFreq = SnapToSpan(gSpanFreq * 0.5);
    UpdateStrings();
    gtk_widget_queue_draw(win);
}

int main (int argc, char *argv[])
{
    r.x=0;
    r.y=0;
    r.width=510;
    r.height = 400;


    // ************************** Thread scheduling **************************88
    // Use round-robin scheduling on a low latency linux kernel.
    // Set application's threads to priority 50.
    // Set data collection thread to priority 99
    // Looks solid on Netbook
 
    pthread_attr_init(&tattr);
    pthread_attr_setschedpolicy(&tattr,SCHED_FIFO);

    pMyHound = new CMySignalHound();

    if(pMyHound->Initialize()) return -5;

    double myTemp = pMyHound->ReadTemperature();
    sprintf(debugstr,"Temp %.2f",myTemp);
    pMyHound->SetupForSweep();        //Set up for initial sweep

     pthread_create(&sweepThread, &tattr,ThreadFunc,NULL); //Begin initial sweep

  /* Initialize GTK+ */
  g_log_set_handler ("Gtk", G_LOG_LEVEL_WARNING, (GLogFunc) gtk_false, NULL);
  gtk_init (&argc, &argv);
  g_log_set_handler ("Gtk", G_LOG_LEVEL_WARNING, g_log_default_handler, NULL);

  /* Create the main window */
  win = gtk_window_new (GTK_WINDOW_TOPLEVEL);
  gtk_container_set_border_width (GTK_CONTAINER (win), 8);
  gtk_window_set_title (GTK_WINDOW (win), "Signal Hound");
  gtk_window_set_position (GTK_WINDOW (win), GTK_WIN_POS_CENTER);
  gtk_widget_realize (win);
  g_signal_connect (win, "destroy", gtk_main_quit, NULL);

  /* Create a layout box with buttons */
  layout = (GtkLayout *) gtk_layout_new (NULL,NULL);
  gtk_layout_set_size(layout,640,410);
  gtk_container_add (GTK_CONTAINER (win), (GtkWidget *) layout);

  graticule = (GtkDrawingArea *) gtk_drawing_area_new();
  gtk_widget_set_size_request((GtkWidget *)graticule,510,400);
  gtk_layout_put ( layout, (GtkWidget *)graticule, 2, 2);



  gtk_signal_connect (GTK_OBJECT (graticule), "expose_event",
                      (GtkSignalFunc) expose_event, NULL);
  gtk_signal_connect (GTK_OBJECT(graticule),"configure_event",
                      (GtkSignalFunc) configure_event, NULL);

  gtk_widget_set_events ((GtkWidget *)graticule, GDK_EXPOSURE_MASK);



  gtk_widget_set_size_request(win,640,410);

  button = gtk_button_new_with_label("FREQ+");
  g_signal_connect (G_OBJECT (button), "clicked", G_CALLBACK (freqplus), (gpointer) win);
  gtk_layout_put ( layout, button, 520, 10);

  button = gtk_button_new_with_label("FREQ-");
  g_signal_connect (G_OBJECT (button), "clicked", G_CALLBACK (freqminus), (gpointer) win);
  gtk_layout_put ( layout, button, 572, 10);

  button = gtk_button_new_with_label("SPAN+");
  g_signal_connect (G_OBJECT (button), "clicked", G_CALLBACK (spanplus), (gpointer) win);
  gtk_layout_put ( layout, button, 520, 110);

  button = gtk_button_new_with_label("SPAN-");
  g_signal_connect (G_OBJECT (button), "clicked", G_CALLBACK (spanminus), (gpointer) win);
  gtk_layout_put ( layout, button, 572, 110);

  button = gtk_button_new_with_label("REF+");
  g_signal_connect (G_OBJECT (button), "clicked", G_CALLBACK (refplus), (gpointer) win);
  gtk_layout_put ( layout, button, 520, 210);

  button = gtk_button_new_with_label("REF-");
  g_signal_connect (G_OBJECT (button), "clicked", G_CALLBACK (refminus), (gpointer) win);
  gtk_layout_put ( layout, button, 572, 210);


  fbuf = gtk_text_buffer_new(NULL);
  rbuf = gtk_text_buffer_new(NULL);
  sbuf = gtk_text_buffer_new(NULL);
  bbuf = gtk_text_buffer_new(NULL);
  dbuf = gtk_text_buffer_new(NULL);

  textfreq = gtk_text_view_new();
  textspan = gtk_text_view_new();
  textreflvl = gtk_text_view_new();
  textRBW = gtk_text_view_new();
  textdebug = gtk_text_view_new();

  UpdateStrings();

  gtk_text_view_set_buffer((GtkTextView *)textfreq,fbuf);
  gtk_text_view_set_buffer((GtkTextView *)textspan,sbuf);
  gtk_text_view_set_buffer((GtkTextView *)textreflvl,rbuf);
  gtk_text_view_set_buffer((GtkTextView *)textRBW,bbuf);
  gtk_text_view_set_buffer((GtkTextView *)textdebug,dbuf);

  gtk_layout_put ( layout, textfreq, 520, 60);
  gtk_layout_put ( layout, textspan, 520, 160);
  gtk_layout_put ( layout, textreflvl, 520, 260);
  gtk_layout_put ( layout, textRBW, 520, 310);
  gtk_layout_put ( layout, textdebug, 520, 360);

  // pMyHound->m_settings.m_suppressImage = false; // if you want image rejection off...

  g_timeout_add(25,(GSourceFunc) TimerFunc,NULL);
    /* Enter the main loop */
  gtk_widget_show_all (win);
  gtk_main ();

delete pMyHound;
  return 0;
}

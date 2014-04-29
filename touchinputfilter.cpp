#include "touchinputfilter.h"
#include "touchapplication.h"

#include <QApplication>
#include <QDesktopWidget>
#include <QWindow>
#include <QScreen>

#ifdef Q_OS_WIN

// Wintab support missing on Windows from Qt 5.0 until Qt 5.2 ... and is broken in Qt 5.2+
#if QT_VERSION >= 0x050000 // && QT_VERSION < 0x050200
#define USE_WINTAB 1
#endif

// WM_POINTER functions
typedef BOOL (WINAPI *PtrGetPointerInfo)(UINT32, POINTER_INFO*);
typedef BOOL (WINAPI *PtrGetPointerFrameInfo)(UINT32, UINT32*, POINTER_INFO*);
typedef BOOL (WINAPI *PtrGetPointerPenInfo)(UINT32, POINTER_PEN_INFO*);
typedef BOOL (WINAPI *PtrGetPointerPenInfoHistory)(UINT32, UINT32*, POINTER_PEN_INFO*);
typedef BOOL (WINAPI *PtrInjectTouchInput)(UINT32, POINTER_TOUCH_INFO*);
typedef BOOL (WINAPI *PtrInitializeTouchInjection)(UINT32, DWORD);

static PtrGetPointerInfo GetPointerInfo;
static PtrGetPointerFrameInfo GetPointerFrameInfo;
static PtrGetPointerPenInfo GetPointerPenInfo;
static PtrGetPointerPenInfoHistory GetPointerPenInfoHistory;
static PtrInjectTouchInput InjectTouchInput;
static PtrInitializeTouchInjection InitializeTouchInjection;

static qreal HimetricToPix;
#define MAX_N_POINTERS 10
static POINTER_INFO pointerInfo[MAX_N_POINTERS];
static POINTER_PEN_INFO penPointerInfo[MAX_N_POINTERS];

#ifdef USE_WINTAB
#include <windows.h>
#include "wintab/utils.h"
#include "wintab/msgpack.h"

// these defines are required for pktdef.h
//#define PACKETDATA (PK_X | PK_Y | PK_BUTTONS | PK_NORMAL_PRESSURE | PK_CURSOR)
// just match Qt for now for testing on Qt 4 (I think there may be a conflict)
#define PACKETDATA (PK_X | PK_Y | PK_BUTTONS | PK_NORMAL_PRESSURE | PK_TANGENT_PRESSURE | PK_ORIENTATION | PK_CURSOR | PK_Z)
#define PACKETMODE 0
#include "wintab/pktdef.h"

static const int PACKET_BUFF_SIZE = 128;
static PACKET localPacketBuf[PACKET_BUFF_SIZE];
static HCTX hTab;

static int minPressure, maxPressure;
static int minX, maxX, minY, maxY;
static DWORD btnPrev;
static QRect desktopArea;

static void initWinTab(HWND hWnd)
{
  // attempt to load wintab
  if(!LoadWintab() || !gpWTInfoA || !gpWTInfoA(0, 0, NULL) || !gpWTPacketsGet)
    return;

  LOGCONTEXTA lcMine;
  gpWTInfoA(WTI_DEFCONTEXT, 0, &lcMine);

  lcMine.lcOptions |= CXO_MESSAGES;
  lcMine.lcPktData = PACKETDATA;
  lcMine.lcMoveMask = PACKETDATA;
  lcMine.lcPktMode = PACKETMODE;
  lcMine.lcBtnUpMask = lcMine.lcBtnDnMask;

  // output coord range
  lcMine.lcOutOrgX = 0;
  lcMine.lcOutExtX = lcMine.lcInExtX;
  lcMine.lcOutOrgY = 0;
  lcMine.lcOutExtY = lcMine.lcInExtY;

  minX = 0;
  maxX = lcMine.lcOutExtX;
  minY = 0;
  maxY = lcMine.lcOutExtY;

  hTab = gpWTOpenA(hWnd, &lcMine, TRUE);

  // set queue size
  gpWTQueueSizeSet(hTab, PACKET_BUFF_SIZE);
}

// Wintab references:
//  http://www.wacomeng.com/windows/index.html (esp. "Documentation" link)
//  Qt: http://qt.gitorious.org/qt/qt/blobs/raw/4.8/src/gui/kernel/qapplication_win.cpp  qwidget_win.cpp
// WM_POINTER references:
//  http://msdn.microsoft.com/en-us/library/windows/desktop/hh454916(v=vs.85).aspx
//  http://software.intel.com/en-us/articles/comparing-touch-coding-techniques-windows-8-desktop-touch-sample
// Windows' "interaction context" stuff can be used with WM_POINTER to recognize gestures (see the
//  InteractionContextSample from Intel) but is a bit too high level, e.g., no way to select single finger
//  pan vs. two finger pan.  Also, we already have "gesture recognition" code

// WM_POINTER observations (from Win 8.0 on Microsoft Surface Pro):
// * Multiple touch points: we receive a move event with the two pointers before the press event for the second
// * Pen entering proximity when touch points are down: WM_POINTERUP is sent for each
//  touch point, but each GetPointerFrameInfo only returns a frame with one point, even for the first point
//  to go up.  WM_POINTERENTER is sent for the pen pointer AFTER touch points go up.
// The POINTER_INFO associated with the pointer up event is identical to a normal pointer up event,
//  except the POINTER_FLAG_CONFIDENCE flag is set, but there is no reason to think this is a reliable
//  indicator across devices.
// If we want to use relative timing, compare touch pointer up time to WM_POINTERENTER time

static bool processWTPacket(MSG* msg)
{
  // primary barrel button is 0x2 for single button pen, but 0x4 with default config of two button pen!
  static DWORD tipBtn = 0x00000001;
  int numPackets = gpWTPacketsGet((HCTX)(msg->lParam), PACKET_BUFF_SIZE, &localPacketBuf);
  for(int ii = 0; ii < numPackets; ii++) {
    DWORD btnNew = localPacketBuf[ii].pkButtons;
    QEvent::Type eventtype = QEvent::TabletMove;
    if((btnNew & tipBtn) && !(btnPrev & tipBtn))
      eventtype = QEvent::TabletPress;
    else if(!(btnNew & tipBtn) && (btnPrev & tipBtn))
      eventtype = QEvent::TabletRelease;
    btnPrev = btnNew;

    POINT ptNew;
    ptNew.x = localPacketBuf[ii].pkX;
    ptNew.y = localPacketBuf[ii].pkY;
    // let's assume maxX, mayY are positive (Qt scaleCoords handles negative case too)
    qreal globalx = ((ptNew.x - minX) * desktopArea.width() / qreal(maxX - minX)) + desktopArea.left();
    qreal globaly = ((ptNew.y - minY) * desktopArea.height() / qreal(maxY - minY)) + desktopArea.top();
    qreal pressure = btnNew ? localPacketBuf[ii].pkNormalPressure / qreal(maxPressure - minPressure) : 0;
    // supposedly this is supposed to be checked on WT_PROXIMITY
    QTabletEvent::PointerType ptrtype
        = (localPacketBuf[ii].pkCursor % 3 == 2) ? QTabletEvent::Eraser : QTabletEvent::Pen;

    int uniqueId = 1;
    TouchInputFilter::instance()->notifyTabletEvent(
        eventtype, QPointF(globalx, globaly), pressure, ptrtype, btnNew & ~tipBtn, uniqueId);
  }
  return true;
}

// Right now, we are stealing Wintab events from Qt's implementation; really, we should use FindWindow and
//  DestroyWindow to get rid of Qt's TabletDummyWindow and create our own invisible window and open our own
//  tablet context to receive Wintab events

static bool winTabEvent(MSG* msg, long* result)
{
  //if(!hTab)
  //  return false;
  switch(msg->message) {
  /* case WM_ACTIVATE:
    gpWTEnable(hTab, GET_WM_ACTIVATE_STATE(msg->wParam, msg->lParam));
    if(GET_WM_ACTIVATE_STATE(msg->wParam, msg->lParam))
      gpWTOverlap(hTab, TRUE);
    break; */
  case WT_PROXIMITY:
    // only handle proximity enter
    if(LOWORD(msg->lParam) != 0) {
      LOGCONTEXTA lc;
      AXIS np;
      // get the current context
      gpWTGetA((HCTX)(msg->wParam), &lc);
      // the following is only needed when stealing events from Qt wintab impl.
      minX = 0;  //lc.lcOutOrgX;
      maxX = lc.lcInExtX - lc.lcInOrgX;  //qAbs(lc.lcOutExtX);
      minY = 0;  //lc.lcOutOrgY;
      maxY = lc.lcInExtY - lc.lcInOrgY;  //qAbs(lc.lcOutExtY);
      // get the size of the pressure axis
      gpWTInfoA(WTI_DEVICES + lc.lcDevice, DVC_NPRESSURE, &np);
      minPressure = int(np.axMin);
      maxPressure = int(np.axMax);
      desktopArea = QApplication::primaryScreen()->virtualGeometry();
      btnPrev = 0;
      //desktopArea = QApplication::desktop()->geometry();
      // TODO: get uniqueID and other cursor info
    }
    return true;
  case WT_PACKET:
    return processWTPacket(msg);
  default:
    break;
  } // switch
  return false; // propagate to next handler
}

#endif // Wintab

static void processPenInfo(const POINTER_PEN_INFO& ppi, QEvent::Type eventtype)
{
  QTabletEvent::PointerType ptrtype
      = (ppi.penFlags & PEN_FLAG_ERASER) ? QTabletEvent::Eraser : QTabletEvent::Pen;

  // unfortunately, there doesn't seem to be any reliable way to figure out himetric to pixel mapping,
  //  so just calculate it from first point we see
  const POINT pix = ppi.pointerInfo.ptPixelLocation;
  const POINT him = ppi.pointerInfo.ptHimetricLocation;
  qreal x = him.x*HimetricToPix;
  qreal y = him.y*HimetricToPix;
  if(pix.x < x - 1 || pix.x > x + 1 || pix.y < y - 1 || pix.y > y + 1) {
    HimetricToPix = qreal(pix.x)/him.x;
    x = him.x*HimetricToPix;
    y = him.y*HimetricToPix;
  }
  // Confirmed that HIMETRIC is higher resolution than pixel location on Surface Pro: saw different HIMETRIC
  //  locations for the same pixel loc, including updates to HIMETRIC loc with no change in pixel loc
  //qDebug("Pix: %d %d; HIMETRIC: %d %d", pix.x, pix.y, him.x, him.y);

  TouchInputFilter::instance()->notifyTabletEvent(eventtype, QPointF(x, y), ppi.pressure/1024.0, ptrtype,
      ppi.penFlags & PEN_FLAG_BARREL, int(ppi.pointerInfo.sourceDevice));
}

// ideally, we wouldn't process history unless mode is STROKE
static void processPenHistory(UINT32 ptrid)
{
  UINT32 historycount = MAX_N_POINTERS;
  POINTER_PEN_INFO* ppi = &penPointerInfo[0];
  if(GetPointerPenInfoHistory(ptrid, &historycount, ppi)) {
    if(historycount > MAX_N_POINTERS) {
      // need more room ... we want to get all history at once since it's returned newest first!
      ppi = new POINTER_PEN_INFO[historycount];
      GetPointerPenInfoHistory(ptrid, &historycount, ppi);
    }
    // process items oldest to newest
    for(int ii = historycount - 1; ii >= 0; ii--)
      processPenInfo(ppi[ii], QEvent::TabletMove);
    if(ppi != &penPointerInfo[0])
      delete[] ppi;
  }
}

static bool processPointerFrame(UINT32 ptrid, Qt::TouchPointState eventtype)
{
  UINT32 pointercount = MAX_N_POINTERS;
  if(GetPointerFrameInfo(ptrid, &pointercount, &pointerInfo[0])) {
    QList<QTouchEvent::TouchPoint> pts;
    for(unsigned int ii = 0; ii < pointercount; ii++) {
      if(pointerInfo[ii].pointerType != PT_TOUCH)
        continue;
      QTouchEvent::TouchPoint pt;
      pt.setId(pointerInfo[ii].pointerId);
      pt.setState(pointerInfo[ii].pointerId == ptrid ? eventtype : Qt::TouchPointMoved);
      pt.setScreenPos(QPointF(pointerInfo[ii].ptPixelLocation.x, pointerInfo[ii].ptPixelLocation.y));
      pt.setPressure(1);
      pts.append(pt);
    }
    if(pts.empty())
      return false;
    //event.t = pointerInfo[0].performanceCount;
    TouchInputFilter::instance()->notifyTouchEvent(eventtype, pts);
    return true;
  }
}

static void initWMPointer()
{
  HINSTANCE user32 = LoadLibraryA("user32.dll");
  if(user32) {
    GetPointerInfo = (PtrGetPointerInfo)(GetProcAddress(user32, "GetPointerInfo"));
    GetPointerFrameInfo = (PtrGetPointerFrameInfo)(GetProcAddress(user32, "GetPointerFrameInfo"));
    GetPointerPenInfo = (PtrGetPointerPenInfo)(GetProcAddress(user32, "GetPointerPenInfo"));
    GetPointerPenInfoHistory = (PtrGetPointerPenInfoHistory)(GetProcAddress(user32, "GetPointerPenInfoHistory"));
    InjectTouchInput = (PtrInjectTouchInput)(GetProcAddress(user32, "InjectTouchInput"));
    InitializeTouchInjection = (PtrInitializeTouchInjection)(GetProcAddress(user32, "InitializeTouchInjection"));
  }

  // Attempt to get HIMETRIC to pixel conversion factor; on Surface Pro, result is close, but not quite
  // 1 HIMETRIC = 0.01 mm
  QWidget* screen = QApplication::desktop()->screen(0);
  // this is equiv to GetDeviceCaps(HORZRES)/GetDeviceCaps(HORZSIZE)
  HimetricToPix = screen->width()/qreal(100*screen->widthMM());
}

static bool winInputEvent(MSG* m, long* result)
{
  static UINT32 penPointerId = NULL;
  //static bool scribbling = false;
  // winTabEvent(m, result);
  if(!GetPointerInfo)
    return false;
  switch(m->message) {
  // WM_POINTER:
  // WM_POINTERDOWN with type PT_PEN: ignore all other pointers, use GetPointerPenInfoHistory
  // otherwise, use GetPointerFrameInfo (discard history)
  case WM_POINTERDOWN:
    if(GetPointerInfo(GET_POINTERID_WPARAM(m->wParam), &pointerInfo[0])) {
      if(pointerInfo[0].pointerType == PT_PEN) {
        penPointerId = pointerInfo[0].pointerId;
        if(GetPointerPenInfo(penPointerId, &penPointerInfo[0]))
          processPenInfo(penPointerInfo[0], QEvent::TabletPress);
        return true;
      }
      else
        return processPointerFrame(GET_POINTERID_WPARAM(m->wParam), Qt::TouchPointPressed);
    }
    break;
  case WM_POINTERUPDATE:
    if(penPointerId && penPointerId == GET_POINTERID_WPARAM(m->wParam)) {
      processPenHistory(penPointerId);
      return true;
    }
    else
      return processPointerFrame(GET_POINTERID_WPARAM(m->wParam), Qt::TouchPointMoved);
    break;
  case WM_POINTERUP:
    if(penPointerId && penPointerId == GET_POINTERID_WPARAM(m->wParam)) {
      if(GetPointerPenInfo(penPointerId, &penPointerInfo[0]))
        processPenInfo(penPointerInfo[0], QEvent::TabletRelease);
      penPointerId = NULL;
      return true;
    }
    else
      return processPointerFrame(GET_POINTERID_WPARAM(m->wParam), Qt::TouchPointReleased);
  default:
    break;
  }
  return false;
}

void initInput()
{
  GetPointerInfo = NULL;
  initWMPointer();
  // Qt 4 supports tablet input
#ifdef USE_WINTAB
  hTab = NULL;
  //initWinTab();
  LoadWintab();
#endif
}

WinInputFilter::WinInputFilter()
{
  initInput();
}

bool WinInputFilter::nativeEventFilter(const QByteArray& eventType, void* message, long* result)
{
  return
#ifdef USE_WINTAB
    winTabEvent((MSG*)message,  result) ||
#endif
    winInputEvent((MSG*)message,  result);
}

#endif // Q_OS_WIN


TouchInputFilter* TouchInputFilter::m_instance = NULL;

TouchInputFilter::TouchInputFilter() : tabletTarget(NULL), touchTarget(NULL)
{
  touchApp = static_cast<TouchApplication*>(QApplication::instance());
  m_instance = this;
  helperObject = new TouchHelperObject;
  // using a QTouchEvent with NULL touch device results in crash
  touchDevice.setName("WM_POINTER");
  touchDevice.setType(QTouchDevice::TouchScreen);
  touchDevice.setCapabilities(QTouchDevice::Position | QTouchDevice::Pressure);
}

TouchInputFilter::~TouchInputFilter()
{
  delete helperObject;
}

// functions for direct injection of tablet and touch events (only used on Windows at the moment)

void TouchInputFilter::notifyTabletEvent(QEvent::Type eventtype,
    const QPointF& globalpos, qreal pressure, QTabletEvent::PointerType ptrtype, int buttons, int deviceid)
{
  if(eventtype == QEvent::TabletPress || !tabletTarget) {
    tabletTarget = QGuiApplication::topLevelAt(globalpos.toPoint());
    if(!tabletTarget)
      return;
    QObject::connect(tabletTarget, SIGNAL(destroyed()), helperObject, SLOT(tabletWindowDestroyed()));
  }
  QWindow* window = tabletTarget;
  if(eventtype == QEvent::TabletRelease) {
    QObject::disconnect(tabletTarget, SIGNAL(destroyed()), helperObject, SLOT(tabletWindowDestroyed()));
    tabletTarget = NULL;
  }

  QPointF localpos = window->mapFromGlobal(globalpos.toPoint()) + (globalpos - globalpos.toPoint());
  QTabletEvent tabletevent(eventtype, localpos, globalpos, deviceid , ptrtype,
                           pressure, 0, 0, 0, 0, 0, QApplication::keyboardModifiers(), deviceid);
  touchApp->setTabletButtons(buttons);
  touchApp->notify(window, &tabletevent);
}

void TouchInputFilter::notifyTouchEvent(
    Qt::TouchPointStates touchstate, const QList<QTouchEvent::TouchPoint>& _points)
{
  QList<QTouchEvent::TouchPoint> points = _points;
  QEvent::Type evtype = QEvent::TouchUpdate;
  if(touchstate == Qt::TouchPointPressed && !touchTarget) {
    touchTarget = QGuiApplication::topLevelAt(points[0].screenPos().toPoint());
    if(touchTarget)
      QObject::connect(touchTarget, SIGNAL(destroyed()), helperObject, SLOT(touchWindowDestroyed()));
    evtype = QEvent::TouchBegin;
  }
  if(!touchTarget)
    return;
  QWindow* window = touchTarget;
  if(touchstate == Qt::TouchPointReleased && points.count() == 1) {
    QObject::disconnect(touchTarget, SIGNAL(destroyed()), helperObject, SLOT(touchWindowDestroyed()));
    touchTarget = NULL;
    evtype = QEvent::TouchEnd;
  }
  if(points.count() > 1)
    touchstate |= Qt::TouchPointMoved;

  for(int ii = 0; ii < points.count(); ++ii) {
    points[ii].setPos(window->mapFromGlobal(points[ii].screenPos().toPoint()));
    // TODO: handle last, start position stuff by saving previous list of touch points
  }

  QTouchEvent touchevent(evtype, &touchDevice, QApplication::keyboardModifiers(), touchstate, points);
  touchApp->notify(window, &touchevent);
}

void TouchHelperObject::tabletWindowDestroyed()
{
  TouchInputFilter::instance()->tabletTarget = NULL;
}

void TouchHelperObject::touchWindowDestroyed()
{
  TouchInputFilter::instance()->touchTarget = NULL;
}


// see http://code.msdn.microsoft.com/windowsdesktop/Touch-Injection-Sample-444d9bf7/
/* #ifdef SCRIBBLE_TEST
bool ScribbleInput::injectTouch(Dim x, Dim y, Dim p, int event)
{
  static bool touchInjectionInited = false;
  static POINTER_TOUCH_INFO contact;
  if(!GetPointerInfo)
    return false;

  if(!touchInjectionInited) {
    InitializeTouchInjection(10, TOUCH_FEEDBACK_NONE);
    memset(&contact, 0, sizeof(POINTER_TOUCH_INFO));
    contact.pointerInfo.pointerType = PT_TOUCH; // we're sending touch input
    contact.pointerInfo.pointerId = 0;          // contact 0
    contact.touchFlags = TOUCH_FLAG_NONE;
    contact.touchMask = TOUCH_MASK_CONTACTAREA | TOUCH_MASK_ORIENTATION | TOUCH_MASK_PRESSURE;
    contact.orientation = 90;
    // set the contact area depending on thickness
    //contact.rcContact.top = 480 - 2;
    //contact.rcContact.bottom = 480 + 2;
    //contact.rcContact.left = 640 - 2;
    //contact.rcContact.right = 640 + 2;
    touchInjectionInited = true;
  }

  contact.pointerInfo.ptPixelLocation.x = x;
  contact.pointerInfo.ptPixelLocation.y = y;
  //contact.pointerInfo.ptHimetricLocation.x = x/HimetricToPix;
  //contact.pointerInfo.ptHimetricLocation.y = y/HimetricToPix;
  contact.pressure = p * 1024;
  if(event == INPUTEVENT_PRESS)
    contact.pointerInfo.pointerFlags = POINTER_FLAG_DOWN | POINTER_FLAG_INRANGE | POINTER_FLAG_INCONTACT;
  else if(event == INPUTEVENT_RELEASE)
    contact.pointerInfo.pointerFlags = POINTER_FLAG_UP;
  else
    contact.pointerInfo.pointerFlags = POINTER_FLAG_UPDATE | POINTER_FLAG_INRANGE | POINTER_FLAG_INCONTACT;
  if(InjectTouchInput(1, &contact) == 0)
    return false;
  // seems to be necessary for us to receive the touch event
  QApplication::processEvents();
  return true;
}
#endif */

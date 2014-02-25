#include "touchapplication.h"

#include <QWindow>
#include <QWidget>
#include <QTabletEvent>


int TouchApplication::m_tabletButtons = 0;

TouchApplication::TouchApplication(int& argc, char** argv) : QApplication(argc, argv), inputState(None)
{
  // prevent Qt from handling touch to mouse translation
  QCoreApplication::setAttribute(Qt::AA_SynthesizeMouseForUnhandledTouchEvents, false);
  acceptCount = 0;
}

bool TouchApplication::sendMouseEvent(QObject* receiver, QEvent::Type mevtype, QPoint globalpos, Qt::KeyboardModifiers modifiers)
{
  QPoint localpos = globalpos;
  if(receiver->isWidgetType())
    localpos = static_cast<QWidget*>(receiver)->mapFromGlobal(globalpos);
  else if(receiver->isWindowType())
    localpos = static_cast<QWindow*>(receiver)->mapFromGlobal(globalpos);

  QMouseEvent* mouseevent = new QMouseEvent(mevtype, localpos, globalpos,
      mevtype == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton,
      mevtype == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton, modifiers);
  if(mevtype == QEvent::MouseButtonRelease) {
    // set low priority to ensure release event is processed after any potential events generated by press
    // another option might be to call processEvents() before postEvent() or notify() for release event
    postEvent(receiver, mouseevent, Qt::LowEventPriority);
    // send an offscreen move event with no buttons (i.e., hover) to workaround problems with press-drag-release on menus
    QPoint offscreen(-10000, -10000);
    QMouseEvent* ev2 = new QMouseEvent(QEvent::MouseMove, offscreen, offscreen, Qt::NoButton, Qt::NoButton, modifiers);
    postEvent(receiver, ev2, Qt::LowEventPriority);
  }
  else
    postEvent(receiver, mouseevent);
  //return QApplication::notify(receiver, &mouseevent);
  return true;
}

// override QApplication::notify() for greatest control over event handling
bool TouchApplication::notify(QObject* receiver, QEvent* event)
{
  QEvent::Type evtype = event->type();
  // first, try to pass TabletPress/TouchBegin event and see if anyone accepts it
  // In Qt, events are first sent to a QWindow, which then figures out what widget they should be sent to.
  // Unfortunately, QWindow event handler always returns true and doesn't change accepted state of event (it
  //  sends a copy of the event and discards the accepted state of the copy), so we must save result from
  //  sending event to final widget (by incrementing acceptCount)
  // When faking mouse events, we must send them to the QWindow instead of a widget, since some of the
  //  routing logic is there, e.g., for handling popup windows
  if((evtype == QEvent::TabletPress || evtype == QEvent::TouchBegin) && inputState == None) {
    if(receiver->isWindowType()) {
      int prevacceptcount = acceptCount;
      QApplication::notify(receiver, event);
      if(acceptCount > prevacceptcount) {
        acceptCount = prevacceptcount;
        inputState = PassThru;
        return true;
      }
      // else, fall through and resend as mouse event
    }
    else {
      event->setAccepted(false);
      bool res = QApplication::notify(receiver, event);
      if(event->isAccepted())
        acceptCount++;
      return res;
    }
  }

  switch(evtype) {
  // reject external mouse events if we are translating touch or tablet input
  case QEvent::MouseButtonRelease:
  case QEvent::MouseMove:
  case QEvent::MouseButtonPress:
    // QWidgetWindow always forwards mouse event to widget as spontaneous event (why?)
    if(inputState != None && event->spontaneous() && receiver->isWindowType())
      return true;   // qDebug("This event should be rejected!");
    break;
  case QEvent::TabletRelease:
    if(inputState == PassThru)
      inputState = None;
  case QEvent::TabletMove:
  case QEvent::TabletPress:
  {
    QTabletEvent* tabletevent = static_cast<QTabletEvent*>(event);
    QEvent::Type mevtype = QEvent::MouseMove;
    if(inputState == None && evtype == QEvent::TabletPress) {
      mevtype = QEvent::MouseButtonPress;
      inputState = TabletInput;
    }
    else if(inputState != TabletInput)  // this covers PassThru
      break;
    if(evtype == QEvent::TabletRelease) {
      mevtype = QEvent::MouseButtonRelease;
      inputState = None;
    }
    return sendMouseEvent(receiver, mevtype, tabletevent->globalPos(), tabletevent->modifiers());
  }
#ifdef QT_5
  case QEvent::TouchCancel:
    evtype = QEvent::TouchEnd;
#endif
  case QEvent::TouchEnd:
    if(inputState == PassThru) // && touchPoints.count() == 1)
      inputState = None;
  case QEvent::TouchUpdate:
  case QEvent::TouchBegin:
  {
    QTouchEvent* touchevent = static_cast<QTouchEvent*>(event);
    QEvent::Type mevtype = QEvent::MouseMove;
    if(inputState == None && evtype == QEvent::TouchBegin
        && touchevent->touchPoints().size() == 1 && touchevent->device()->type() != QTouchDevice::TouchPad) {
      activeTouchId = touchevent->touchPoints().first().id();
      mevtype = QEvent::MouseButtonPress;
      inputState = TouchInput;
    }
    else if(inputState != TouchInput)  // this covers PassThru
      break;
    if(evtype == QEvent::TouchEnd)
      inputState = None;
    event->setAccepted(true);
    QList<QTouchEvent::TouchPoint> touchPoints = touchevent->touchPoints();
    for(int ii = 0; ii < touchPoints.count(); ++ii) {
      const QTouchEvent::TouchPoint& touchpt = touchPoints.at(ii);
      if(touchpt.id() == activeTouchId) {
        if(touchpt.state() == Qt::TouchPointReleased) {
          mevtype = QEvent::MouseButtonRelease;
          activeTouchId = -1;
        }
        return sendMouseEvent(receiver, mevtype, touchpt.screenPos().toPoint(), touchevent->modifiers());
      }
    }
    // swallow all touch events until TouchEnd
    // another option would be to propagate the touch event with the activeTouchId point removed, if >1 point
    return true;
  }
  default:
    break;
  }
  return QApplication::notify(receiver, event);
}

/* bool TouchApplication::winEventFilter(MSG* m, long* result) {
  return ScribbleInput::winEventFilter(m, result);
  // return QApplication::winEventFilter(m, result );
} */

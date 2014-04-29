#ifndef TOUCHINPUTFILTER_H
#define TOUCHINPUTFILTER_H

#include <QAbstractNativeEventFilter>
#include <QTabletEvent>


class TouchApplication;

class TouchHelperObject : public QObject
{
  Q_OBJECT

private slots:
  void tabletWindowDestroyed();
  void touchWindowDestroyed();
};

class TouchInputFilter : public QAbstractNativeEventFilter
{
  friend class TouchHelperObject;
public:
  TouchInputFilter();
  ~TouchInputFilter();

  static TouchInputFilter* instance() { return m_instance; }
  void notifyTouchEvent(Qt::TouchPointStates touchstate, const QList<QTouchEvent::TouchPoint>& _points);
  void notifyTabletEvent(QEvent::Type eventtype,
      const QPointF& globalpos, qreal pressure, QTabletEvent::PointerType ptrtype, int buttons, int deviceid);

protected:
  QWindow* tabletTarget;
  QWindow* touchTarget;
  TouchApplication* touchApp;
  QTouchDevice touchDevice;
  TouchHelperObject* helperObject;

  static TouchInputFilter* m_instance;
};

#ifdef Q_OS_WIN
#include "wintab/wmpointer.h"

class WinInputFilter : public TouchInputFilter
{
public:
  WinInputFilter();
  bool nativeEventFilter(const QByteArray& eventType, void* message, long* result);
};

#endif  // Q_OS_WIN

#endif

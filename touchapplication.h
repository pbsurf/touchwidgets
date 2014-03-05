#ifndef TOUCHAPPLICATION_H
#define TOUCHAPPLICATION_H

#include <QApplication>

class TouchApplication : public QApplication
{
public:
  TouchApplication(int& argc, char** argv);

  bool notify(QObject* receiver, QEvent* event);

  static int tabletButtons() { return m_tabletButtons; }
  static void setTabletButtons(int btns) { m_tabletButtons = btns; }

private:
  bool sendMouseEvent(QObject* receiver, QEvent::Type mevtype, QPoint globalpos, Qt::KeyboardModifiers modifiers);
  QObject* getRecvWindow(QObject* candidate);

  int activeTouchId;
  int acceptCount;
  enum {None, PassThru, TouchInput, TabletInput} inputState;
  static int m_tabletButtons;
};

#endif

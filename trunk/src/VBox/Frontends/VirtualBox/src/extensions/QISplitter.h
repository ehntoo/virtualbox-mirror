/** @file
 *
 * VBox frontends: Qt GUI ("VirtualBox"):
 * VirtualBox Qt extensions: QISplitter class declaration
 */

/*
 * Copyright (C) 2009 Oracle Corporation
 *
 * This file is part of VirtualBox Open Source Edition (OSE), as
 * available from http://www.virtualbox.org. This file is free software;
 * you can redistribute it and/or modify it under the terms of the GNU
 * General Public License (GPL) as published by the Free Software
 * Foundation, in version 2 as it comes in the "COPYING" file of the
 * VirtualBox OSE distribution. VirtualBox OSE is distributed in the
 * hope that it will be useful, but WITHOUT ANY WARRANTY of any kind.
 */

#ifndef _QISplitter_h_
#define _QISplitter_h_

/* Global includes */
#include <QSplitter>

/* Global forwardes */
class SplitterHandle;

class QISplitter : public QSplitter
{
    Q_OBJECT;

public:

    QISplitter (QWidget *aParent);

private:

    bool eventFilter (QObject *aWatched, QEvent *aEvent);
    void showEvent (QShowEvent *aEvent);

    QSplitterHandle* createHandle();

    QByteArray mBaseState;

    bool mPolished;
};

class QISplitterHandle : public QSplitterHandle
{
    Q_OBJECT;

public:

    QISplitterHandle (Qt::Orientation aOrientation, QISplitter *aParent);

private:

    void paintEvent (QPaintEvent *aEvent);
};

#endif /* _QISplitter_h_ */


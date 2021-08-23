/* $Id$ */
/** @file
 * VBox Qt GUI - UIWizardCloneVDPageBasic1 class declaration.
 */

/*
 * Copyright (C) 2006-2020 Oracle Corporation
 *
 * This file is part of VirtualBox Open Source Edition (OSE), as
 * available from http://www.virtualbox.org. This file is free software;
 * you can redistribute it and/or modify it under the terms of the GNU
 * General Public License (GPL) as published by the Free Software
 * Foundation, in version 2 as it comes in the "COPYING" file of the
 * VirtualBox OSE distribution. VirtualBox OSE is distributed in the
 * hope that it will be useful, but WITHOUT ANY WARRANTY of any kind.
 */

#ifndef FEQT_INCLUDED_SRC_wizards_clonevd_UIWizardCloneVDPageBasic1_h
#define FEQT_INCLUDED_SRC_wizards_clonevd_UIWizardCloneVDPageBasic1_h
#ifndef RT_WITHOUT_PRAGMA_ONCE
# pragma once
#endif

/* Qt includes: */
#include <QSet>

/* GUI includes: */
#include "UINativeWizardPage.h"

/* COM includes: */
#include "COMEnums.h"
#include "CMediumFormat.h"

/* Forward declarations: */
class QIRichTextLabel;
class UIDiskFormatsGroupBox;
class UIWizardCloneVD;


/** 2nd page of the Clone Virtual Disk Image wizard (basic extension): */
class UIWizardCloneVDPageBasic1 : public UINativeWizardPage
{
    Q_OBJECT;

public:

    /** Constructs basic page.
      * @param  enmDeviceType  Brings the device type to limit format to. */
    UIWizardCloneVDPageBasic1(KDeviceType enmDeviceType);

private slots:

    void sltMediumFormatChanged();

private:

    UIWizardCloneVD *cloneWizard() const;

    /** Handles translation event. */
    virtual void retranslateUi() /* override */;
    void prepare(KDeviceType enmDeviceType);

    /** Prepares the page. */
    virtual void initializePage() /* override */;

    /** Returns whether the page is complete. */
    virtual bool isComplete() const /* override */;

    /** Holds the description label instance. */
    QIRichTextLabel *m_pLabel;
    UIDiskFormatsGroupBox *m_pFormatGroupBox;

    QSet<QString> m_userModifiedParameters;
};

#endif /* !FEQT_INCLUDED_SRC_wizards_clonevd_UIWizardCloneVDPageBasic1_h */

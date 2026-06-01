/*******************************************************************************
*                                                                              *
* diffHighlight.h -- Highlight unified-diff changes in an NEdit window          *
*                                                                              *
*******************************************************************************/
#ifndef NEDIT_DIFFHIGHLIGHT_H_INCLUDED
#define NEDIT_DIFFHIGHLIGHT_H_INCLUDED

#include "nedit.h"

void ApplyDiffFileHighlight(WindowInfo *window, const char *diffFile,
        const char *openedFilePath);

#endif /* NEDIT_DIFFHIGHLIGHT_H_INCLUDED */

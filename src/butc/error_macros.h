/*
 * Copyright 2000, International Business Machines Corporation and others.
 * All Rights Reserved.
 * 
 * This software has been released under the terms of the IBM Public
 * License.  For details, see the LICENSE file in the top-level source
 * directory or online at http://www.openafs.org/dl/license10.html
 */

#define ERROR_EXIT(evalue)                                      \
	{                                                       \
            code = evalue;                                      \
            goto error_exit;                                    \
        }

#define ERROR_EXIT2(evalue)                                     \
	{                                                       \
            code = evalue;                                      \
            goto error_exit2;                                   \
        }

#define ABORT_EXIT(evalue)                                      \
	{                                                       \
            code = evalue;                                      \
            goto abort_exit;                                    \
        }

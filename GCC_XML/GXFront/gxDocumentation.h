/*=========================================================================

  Program:   GCC-XML
  Module:    $RCSfile: gxDocumentation.h,v $
  Language:  C++
  Date:      $Date: 2002-10-30 19:53:12 $
  Version:   $Revision: 1.1 $

  Copyright (c) 2002 Kitware, Inc., Insight Consortium.  All rights reserved.
  See Copyright.txt for details.

     This software is distributed WITHOUT ANY WARRANTY; without even 
     the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR 
     PURPOSE.  See the above copyright notices for more information.

=========================================================================*/
#ifndef _gxDocumentation_h
#define _gxDocumentation_h

#include "gxSystemTools.h"

/** Class to generate documentation.  */
class gxDocumentation
{
public:
  static void PrintUsage(std::ostream& os);
  static void PrintHelp(std::ostream& os);
  static void PrintHelpHTML(std::ostream& os);
  static void PrintManPage(std::ostream& os);
  static void PrintCopyright(std::ostream& os);
  static void PrintColumn(std::ostream& os, int width,
                          const char* indent, const char* text);
};

#endif

/*=========================================================================

  Program:   GCC-XML
  Module:    $RCSfile: gxFlagsParser.h,v $
  Language:  C++
  Date:      $Date: 2003-10-28 20:03:20 $
  Version:   $Revision: 1.3 $

  Copyright (c) 2002 Kitware, Inc., Insight Consortium.  All rights reserved.
  See Copyright.txt for details.

     This software is distributed WITHOUT ANY WARRANTY; without even
     the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
     PURPOSE.  See the above copyright notices for more information.

=========================================================================*/
#ifndef _gxFlagsParser_h
#define _gxFlagsParser_h

#include "gxSystemTools.h"

#include <vector>

/** Class to parse gccxml flags into individual arguments.  */
class gxFlagsParser
{
public:
  /** Parse the given set of flags.  */
  void Parse(const char* flags);

  /** Add the set of arguments to the given vector.  */
  void AddParsedFlags(std::vector<std::string>& resultArgs);

private:
  void AddFlag(const std::string&);

  std::vector<std::string> m_Flags;
};

#endif

// Copyright (C) 2013 Free Software Foundation, Inc.
//
// This file is part of the GNU Application Binary Interface Generic
// Analysis and Instrumentation Library (libabigail).  This library is
// free software; you can redistribute it and/or modify it under the
// terms of the GNU General Public License as published by the Free
// Software Foundation; either version 3, or (at your option) any
// later version.

// This library is distributed in the hope that it will be useful, but
// WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
// General Public License for more details.

// Under Section 7 of GPL version 3, you are granted additional
// permissions described in the GCC Runtime Library Exception, version
// 3.1, as published by the Free Software Foundation.

// You should have received a copy of the GNU General Public License
// and a copy of the GCC Runtime Library Exception along with this
// program; see the files COPYING3 and COPYING.RUNTIME respectively.
// If not, see <http://www.gnu.org/licenses/>.

// -*- Mode: C++ -*-
/// @file

#ifndef __ABG_CONFIG_H__
#define __ABG_CONFIG_H__

namespace abigail
{

/// This type abstracts the configuration information of the library.
class config
{
public:
  config();

  unsigned char
  get_format_minor_version_number() const;

  void
  set_format_minor_version_number(unsigned char);

  unsigned char
  get_format_major_version_number() const;


  void
  set_format_major_version_number(unsigned char);

  unsigned
  get_xml_element_indent() const;

  void
  set_xml_element_indent(unsigned);

private:
  unsigned char m_format_minor;
  unsigned char m_format_major;
  unsigned m_xml_element_indent;
};//end class config

}//end namespace abigail

#endif //__ABG_CONFIG_H__

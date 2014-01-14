// -*- mode: C++ -*-
//
// Copyright (C) 2013 Red Hat, Inc.
//
// This file is part of the GNU Application Binary Interface Generic
// Analysis and Instrumentation Library (libabigail).  This library is
// free software; you can redistribute it and/or modify it under the
// terms of the GNU Lesser General Public License as published by the
// Free Software Foundation; either version 3, or (at your option) any
// later version.

// This library is distributed in the hope that it will be useful, but
// WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
// General Lesser Public License for more details.

// You should have received a copy of the GNU Lesser General Public
// License along with this program; see the file COPYING-LGPLV3.  If
// not, see <http://www.gnu.org/licenses/>.

/// @file
///
/// This file contains the definitions of the entry points to
/// de-serialize an instance of @ref translation_unit from an ABI
/// Instrumentation file in libabigail native XML format.

#include <cstring>
#include <cstdlib>
#include <tr1/unordered_map>
#include <stack>
#include <assert.h>
#include <libxml/xmlstring.h>
#include <libxml/xmlreader.h>
#include "abg-libxml-utils.h"
#include "abg-corpus.h"
#include "abg-libzip-utils.h"

namespace abigail
{

using xml::xml_char_sptr;

/// Internal namespace for reader.
namespace xml_reader
{
using std::string;
using std::stack;
using std::tr1::unordered_map;
using std::tr1::dynamic_pointer_cast;
using std::vector;
using std::istream;
using zip_utils::zip_sptr;
using zip_utils::zip_file_sptr;
using zip_utils::open_archive;
using zip_utils::open_file_in_archive;

class read_context;

static void update_read_context(read_context&);
static void update_read_context(read_context&, xmlNodePtr);
static void update_depth_info_of_read_context(read_context&, int);

/// This abstracts the context in which the current ABI
/// instrumentation dump is being de-serialized.  It carries useful
/// information needed during the de-serialization, but that does not
/// make sense to be stored in the final resulting in-memory
/// representation of ABI Corpus.
class read_context
{
  read_context();

public:

  typedef unordered_map<string,
			shared_ptr<type_base> >::const_iterator
  const_types_map_it;

  typedef unordered_map<string,
			shared_ptr<function_tdecl> >::const_iterator
  const_fn_tmpl_map_it;

  typedef unordered_map<string,
			shared_ptr<class_tdecl> >::const_iterator
  const_class_tmpl_map_it;

  read_context(xml::reader_sptr reader) : m_depth(0), m_reader(reader)
  {}

  int
  get_depth() const
  {return m_depth;}

  void
  set_depth(int d)
  {m_depth = d;}

  xml::reader_sptr
  get_reader() const
  {return m_reader;}

  /// Return the type that is identified by a unique ID.  Note that
  /// for a type to be "identified" by id, the function key_type_decl
  /// must have been previously called with that type and with id.
  ///
  /// @param id the unique id to consider.
  ///
  /// @return the type identified by the unique id id, or a null
  /// pointer if no type has ever been associated with id before.
  shared_ptr<type_base>
  get_type_decl(const string& id) const
  {
    const_types_map_it i = m_types_map.find(id);
    if (i == m_types_map.end())
      return shared_ptr<type_base>();
    return shared_ptr<type_base>(i->second);
  }

  /// Return the function template that is identified by a unique ID.
  ///
  /// Note that for a function template to be identified by id, the
  /// function key_fn_tmpl_decl must have been previously called with
  /// that function template and with id.
  ///
  /// @param id the ID to consider.
  ///
  /// @return the function template identified by id, or a null
  /// pointer if no function template has ever been associated with
  /// id before.
  shared_ptr<function_tdecl>
  get_fn_tmpl_decl(const string& id) const
  {
    const_fn_tmpl_map_it i = m_fn_tmpl_map.find(id);
    if (i == m_fn_tmpl_map.end())
      return shared_ptr<function_tdecl>();
    return i->second;
  }

  /// Return the class template that is identified by a unique ID.
  ///
  /// Note that for a class template to be identified by id, the
  /// function key_class_tmpl_decl must have been previously called
  /// with that class template and with id.
  ///
  /// @param id the ID to consider.
  ///
  /// @return the class template identified by id, or a null pointer
  /// if no class template has ever been associated with id before.
  shared_ptr<class_tdecl>
  get_class_tmpl_decl(const string& id) const
  {
    const_class_tmpl_map_it i = m_class_tmpl_map.find(id);
    if (i == m_class_tmpl_map.end())
      return shared_ptr<class_tdecl>();
    return i->second;
  }

  /// Return the current lexical scope.  For this function to return a
  /// sane result, the path to the current decl element (starting from the
  /// root element) must be up to date.  It is updated by a call to
  /// #update_read_context.
  scope_decl*
  get_cur_scope()
  {
    shared_ptr<decl_base> cur_decl = get_cur_decl();

    if (dynamic_cast<scope_decl*>(cur_decl.get()))
      // The current decl is a scope_decl, so it's our lexical scope.
      return dynamic_pointer_cast<scope_decl>(cur_decl).get();
    else if (cur_decl)
      // The current decl is not a scope_decl, so our lexical scope is
      // the scope of this decl.
      return cur_decl->get_scope();
    else
      // We have no scope set.
      return 0;
  }

  decl_base_sptr
  get_cur_decl() const
  {
    if (m_decls_stack.empty())
      return shared_ptr<decl_base>(static_cast<decl_base*>(0));

    return m_decls_stack.top();
  }

  translation_unit*
  get_translation_unit()
  {
    const global_scope* global = 0;
    if (shared_ptr<decl_base> d = get_cur_decl ())
      global = get_global_scope(d);

    if (global)
      return global->get_translation_unit();

    return 0;
  }

  void
  push_decl(decl_base_sptr d)
  {
    m_decls_stack.push(d);
  }

  decl_base_sptr
  pop_decl()
  {
    if (m_decls_stack.empty())
      return decl_base_sptr();

    shared_ptr<decl_base> t = get_cur_decl();
    m_decls_stack.pop();
    return t;
  }

  void
  clear_type_map()
  {m_types_map.clear();}

  /// Associate an ID with a type.
  ///
  /// @param type the type to associate witht he ID.
  ///
  /// @param id the ID to associate to the type.
  ///
  /// @return true upon successful completion, false otherwise.  Note
  /// that this returns false if the was already associate to an ID
  /// before.
  bool
  key_type_decl(shared_ptr<type_base> type, const string& id)
  {
    assert(type);

    const_types_map_it i = m_types_map.find(id);
    if (i != m_types_map.end())
      return false;

    m_types_map[id] = type;
    return true;
  }

  /// Associate an ID with a type.
  ///
  /// If ID is an id for an existing type, this function replaces the
  /// exising type with the new DEFINITION type passe in argument.
  ///
  /// @param definition the type to associate witht he ID.
  ///
  /// @param id the ID to associate to the type.
  ///
  /// @return true upon successful completion, false otherwise.  Note
  /// that this returns false if the was already associate to an ID
  /// before.
  bool
  key_replacement_of_type_decl(shared_ptr<type_base> definition,
			       const string& id)
  {
    const_types_map_it i = m_types_map.find(id);
    if (i != m_types_map.end())
      m_types_map.erase(i);
    key_type_decl(definition, id);

    return true;
  }

  /// Associate an ID to a function template.
  ///
  /// @param fn_tmpl_decl the function template to consider.
  ///
  /// @param id the ID to associate to the function template.
  ///
  /// @return true upon successful completion, false otherwise.  Note
  /// that the function returns false if an ID was previously
  /// associated to the function template.
  bool
  key_fn_tmpl_decl(shared_ptr<function_tdecl> fn_tmpl_decl,
		   const string& id)
  {
    assert(fn_tmpl_decl);

    const_fn_tmpl_map_it i = m_fn_tmpl_map.find(id);
    if (i != m_fn_tmpl_map.end())
      return false;

    m_fn_tmpl_map[id] = fn_tmpl_decl;
    return true;
  }

    /// Associate an ID to a class template.
  ///
  /// @param class_tmpl_decl the class template to consider.
  ///
  /// @param id the ID to associate to the class template.
  ///
  /// @return true upon successful completion, false otherwise.  Note
  /// that the function returns false if an ID was previously
  /// associated to the class template.
  bool
  key_class_tmpl_decl(shared_ptr<class_tdecl> class_tmpl_decl,
		      const string& id)
  {
    assert(class_tmpl_decl);

    const_class_tmpl_map_it i = m_class_tmpl_map.find(id);
    if (i != m_class_tmpl_map.end())
      return false;

    m_class_tmpl_map[id] = class_tmpl_decl;
    return true;
  }

  /// This function must be called on each declaration that is created during
  /// the parsing.  It adds the declaration to the current scope, and updates
  /// the state of the parsing context accordingly.
  ///
  /// @param decl the newly created declaration.
  void
  push_decl_to_current_scope(shared_ptr<decl_base> decl,
			     bool add_to_current_scope)
  {
    assert(decl);

    if (add_to_current_scope)
      add_decl_to_scope(decl, get_cur_scope());
    push_decl(decl);
  }

  /// This function must be called on each decl that is created during
  /// the parsing.  It adds the decl to the current scope, and updates
  /// the state of the parsing context accordingly.
  ///
  /// @param decl the newly created decl.
  ///
  /// @param node the xml node from which the decl has been created if any.
  ///
  /// @param update_depth_info should be set to true if the function
  /// should update the depth information maintained in the parsing
  /// context.  If the xml element node has been 'hit' by the
  /// advance_cursor then this should be set to false, because that
  /// function updates the depth information maintained in the parsing
  /// context already.
 void
 push_decl_to_current_scope(shared_ptr<decl_base> decl,
			    xmlNodePtr node, bool update_depth_info,
			    bool add_to_current_scope)
  {
    assert(decl);

    if (update_depth_info)
      update_read_context(*this, node);

    push_decl_to_current_scope(decl, add_to_current_scope);
  }

  /// This function must be called on each type decl that is created
  /// during the parsing.  It adds the type decl to the current scope
  /// and associates a unique ID to it.
  ///
  /// @param t type_decl
  ///
  /// @param id the unique ID to be associated to t
  ///
  /// @return true upon successful completion.
  ///
  bool
  push_and_key_type_decl(shared_ptr<type_base> t, const string& id,
			 bool add_to_current_scope)
  {
    shared_ptr<decl_base> decl = dynamic_pointer_cast<decl_base>(t);
    if (!decl)
      return false;

    push_decl_to_current_scope(decl, add_to_current_scope);
    key_type_decl(t, id);
    return true;
  }

  /// This function must be called on each type decl that is created
  /// during the parsing.  It adds the type decl to the current scope
  /// and associates a unique ID to it.
  ///
  /// @param t the type decl to consider.
  ///
  /// @param id the ID to associate to it.
  ///
  /// @param node the xml elment node that t was constructed from.
  ///
  /// @param update_depth_info should be set to true if this
  /// function should update the depth information maintained in the
  /// parsing context.
  ///
  /// @return true upon successful completion, false otherwise.
  bool
  push_and_key_type_decl(shared_ptr<type_base>	t, const string& id,
			 xmlNodePtr node, bool update_depth_info,
			 bool add_to_current_scope)
  {
    if (update_depth_info)
      update_read_context(*this, node);

    return push_and_key_type_decl(t, id, add_to_current_scope);
  }

private:
  // The depth of the current node in the xml tree.
  int m_depth;
  unordered_map<string, shared_ptr<type_base> > m_types_map;
  unordered_map<string, shared_ptr<function_tdecl> > m_fn_tmpl_map;
  unordered_map<string, shared_ptr<class_tdecl> > m_class_tmpl_map;
  xml::reader_sptr m_reader;
  stack<shared_ptr<decl_base> > m_decls_stack;
};

static int	advance_cursor(read_context&);
static bool	read_translation_unit_from_input(read_context&,
						 translation_unit&);
static bool	read_location(read_context&, location&);
static bool	read_location(read_context&, xmlNodePtr, location&);
static bool	read_visibility(xmlNodePtr, decl_base::visibility&);
static bool	read_binding(xmlNodePtr, decl_base::binding&);
static bool	read_access(xmlNodePtr, class_decl::access_specifier&);
static bool	read_size_and_alignment(xmlNodePtr, size_t&, size_t&);
static bool	read_static(xmlNodePtr, bool&);
static bool	read_offset_in_bits(xmlNodePtr, size_t&);
static bool	read_cdtor_const(xmlNodePtr, bool&, bool&, bool&);
static bool	read_is_declaration_only(xmlNodePtr, bool&);
static bool	read_is_virtual(xmlNodePtr, bool&);

// <build a c++ class  from an instance of xmlNodePtr>
//
// Note that whenever a new function to build a type is added here,
// you should make sure to call it from the build_type function, which
// should be the last function of the list of declarated function below.
static shared_ptr<function_decl::parameter>
build_function_parameter (read_context&, const xmlNodePtr);

static shared_ptr<function_decl>
build_function_decl(read_context&, const xmlNodePtr,
		    shared_ptr<class_decl>, bool, bool);

static shared_ptr<var_decl>
build_var_decl(read_context&, const xmlNodePtr, bool, bool);

static shared_ptr<type_decl>
build_type_decl(read_context&, const xmlNodePtr, bool, bool);

static shared_ptr<qualified_type_def>
build_qualified_type_decl(read_context&, const xmlNodePtr, bool, bool);

static shared_ptr<pointer_type_def>
build_pointer_type_def(read_context&, const xmlNodePtr, bool, bool);

static shared_ptr<reference_type_def>
build_reference_type_def(read_context&, const xmlNodePtr, bool, bool);

static shared_ptr<enum_type_decl>
build_enum_type_decl(read_context&, const xmlNodePtr, bool, bool);

static shared_ptr<typedef_decl>
build_typedef_decl(read_context&, const xmlNodePtr, bool, bool);

static shared_ptr<class_decl>
build_class_decl(read_context&, const xmlNodePtr, bool, bool);

static shared_ptr<function_tdecl>
build_function_tdecl(read_context&, const xmlNodePtr, bool, bool);

static shared_ptr<class_tdecl>
build_class_tdecl(read_context&, const xmlNodePtr, bool, bool);

static shared_ptr<type_tparameter>
build_type_tparameter(read_context&, const xmlNodePtr, unsigned, bool);

static shared_ptr<type_composition>
build_type_composition(read_context&, const xmlNodePtr, unsigned, bool);

static shared_ptr<non_type_tparameter>
build_non_type_tparameter(read_context&, const xmlNodePtr, unsigned, bool);

static shared_ptr<template_tparameter>
build_template_tparameter(read_context&, const xmlNodePtr, unsigned, bool);

static shared_ptr<template_parameter>
build_template_parameter(read_context&, const xmlNodePtr, unsigned, bool);

// Please make this build_type function be the last one of the list.
// Note that it should call each type-building function above.  So
// please make sure to update it accordingly, whenever a new
// type-building function is added here.
static shared_ptr<type_base>
build_type(read_context&, const xmlNodePtr, bool, bool);
// </build a c++ class  from an instance of xmlNodePtr>

static bool	handle_element(read_context&);
static bool	handle_type_decl(read_context&);
static bool	handle_namespace_decl(read_context&);
static bool	handle_qualified_type_decl(read_context&);
static bool	handle_pointer_type_def(read_context&);
static bool	handle_reference_type_def(read_context&);
static bool	handle_enum_type_decl(read_context&);
static bool	handle_typedef_decl(read_context&);
static bool	handle_var_decl(read_context&);
static bool	handle_function_decl(read_context&);
static bool	handle_class_decl(read_context&);
static bool	handle_function_tdecl(read_context&);
static bool	handle_class_tdecl(read_context&);

/// Updates the instance of read_context.  Basically update thee path
/// of elements from the root to the current element, that we maintain
/// to know the current scope.  This function needs to be called after
/// each call to xmlTextReaderRead.
///
/// @param ctxt the context to update.
static void
update_read_context(read_context& ctxt)
{
  xml::reader_sptr reader = ctxt.get_reader();

  if (XML_READER_GET_NODE_TYPE(reader) != XML_READER_TYPE_ELEMENT)
    return;

  // Update the depth of the current reader cursor in the reader
  // context.
  int depth = xmlTextReaderDepth(reader.get());
  update_depth_info_of_read_context(ctxt, depth);
}

/// Updates the instance of read_context, from an instance of xmlNode.
/// Basically update thee path of elements from the root to the
/// current element, that we maintain to know the current scope.  This
/// function needs to be called each time a build_xxx builds an C++
/// from an xmlNodePtr.
static void
update_read_context(read_context& ctxt, xmlNodePtr node)
{
  if (node->type != XML_ELEMENT_NODE)
    return;

  int depth = xml::get_xml_node_depth(node);

  if (depth >= 0)
    update_depth_info_of_read_context(ctxt, depth);
}

/// Helper function used by update_read_context.
///
/// Updates the depth information maintained in the read_context.
/// Updates the stack of IR node we maintain to know our current
/// context.
static void
update_depth_info_of_read_context(read_context& ctxt, int new_depth)
{
  int ctxt_depth = ctxt.get_depth();

  if (new_depth > ctxt_depth)
    // we went down the tree.  There is nothing to do until we
    // actually parse the new element.
    ;
  else if (new_depth <= ctxt_depth)
    {
      // we went up the tree or went to a sibbling
      for (int nb = ctxt_depth - new_depth + 1; nb; --nb)
	{
	  shared_ptr<decl_base> d = ctxt.pop_decl();

	  /// OK, this is a hack needed because the libxml reader
	  /// interface doesn't provide us with a reliable way to know
	  /// when we read the end of an XML element.
	  if (is_at_class_scope(d) && nb > 2)
	    // This means we logically poped out at least a member of
	    // a class (i.e, during the xml parsing, we went up so
	    // that we got out of an e.g, member-type, data-member or
	    // member-function xml element.  The issue is that there
	    // are two nested XML elment in that case (e.g,
	    // data-member -> var-decl) to represent just one concrete
	    // c++ type (e.g, the var_decl that is in the class_decl
	    // scope).  So libxml reports that we should pop two *XML*
	    // elements, but we should only pop one *C++* instance
	    // from our stack.
	    nb--;
	}
    }

  ctxt.set_depth(new_depth);
}

/// Moves the xmlTextReader cursor to the next xml node in the input
/// document.  Return 1 of the parsing was successful, 0 if no input
/// xml token is left, or -1 in case of error.
///
/// @param ctxt the read context
///
static int
advance_cursor(read_context& ctxt)
{
  xml::reader_sptr reader = ctxt.get_reader();
  int status = xmlTextReaderRead(reader.get());
  if (status == 1)
    update_read_context(ctxt);

  return status;
}

/// Parse the input XML document containing a translation_unit,
/// represented by an 'abi-instr' element node, associated to the current
/// context.
///
/// @param ctxt the current input context
///
/// @param tu the translation unit resulting from the parsing.
///
/// @return true upon successful parsing, false otherwise.
static bool
read_translation_unit_from_input(read_context&	ctxt,
				 translation_unit&	tu)
{
  xml::reader_sptr reader = ctxt.get_reader();
  if (!reader)
    return false;

  // The document must start with the abi-instr node.
  int status = 1;
  while (status == 1
	 && XML_READER_GET_NODE_TYPE(reader) != XML_READER_TYPE_ELEMENT)
    status = advance_cursor (ctxt);

  if (status != 1 || !xmlStrEqual (XML_READER_GET_NODE_NAME(reader).get(),
				   BAD_CAST("abi-instr")))
    return false;

  ctxt.clear_type_map();

  xml::xml_char_sptr addrsize_str = XML_READER_GET_ATTRIBUTE(reader,
							     "address-size");
  if (addrsize_str)
    {
      char address_size = atoi(reinterpret_cast<char*>(addrsize_str.get()));
      tu.set_address_size(address_size);
    }

  xml::xml_char_sptr path_str = XML_READER_GET_ATTRIBUTE(reader, "path");
  if (path_str)
    tu.set_path(reinterpret_cast<char*>(path_str.get()));

  // We are at global scope, as we've just seen the top-most
  // "abi-instr" element.
  ctxt.push_decl(tu.get_global_scope());

  for (status = advance_cursor(ctxt);
       (status == 1
	// There must be at least one decl pushed in the context
	// during the parsing.
	&& ctxt.get_cur_decl());
       status = advance_cursor(ctxt))
    {
      xmlReaderTypes node_type = XML_READER_GET_NODE_TYPE(reader);

      switch (node_type)
	{
	case XML_READER_TYPE_ELEMENT:
	  if (!handle_element(ctxt))
	    abort();
	    //return false;
	  break;
	default:
	  break;
	}
    }

  if (status != -1)
    return true;
  return false;
}

/// Parse the input XML document containing an ABI corpus, represented
/// by an 'abi-corpus' element node, associated to the current
/// context.
///
/// @param ctxt the current input context.
///
/// @param corp the corpus resulting from the parsing.  This is set
/// iff the function returns true.
///
/// @return true upon successful parsing, false otherwise.
static bool
read_corpus_from_input(read_context&	ctxt,
		       corpus& corp)
{
  xml::reader_sptr reader = ctxt.get_reader();
  if (!reader)
    return false;

  // The document must start with the abi-corpus node.
  int status = 1;
  while (status == 1
	 && XML_READER_GET_NODE_TYPE(reader) != XML_READER_TYPE_ELEMENT)
    status = advance_cursor (ctxt);

  if (status != 1 || !xmlStrEqual (XML_READER_GET_NODE_NAME(reader).get(),
				   BAD_CAST("abi-corpus")))
    return false;

  xml::xml_char_sptr path_str = XML_READER_GET_ATTRIBUTE(reader, "path");

  if (path_str)
    corp.set_path(reinterpret_cast<char*>(path_str.get()));

  // Advance the cursor until the next 'abi-instr' element.
  do
    status = advance_cursor (ctxt);
  while (status == 1
	 && XML_READER_GET_NODE_TYPE(reader) != XML_READER_TYPE_ELEMENT);

  bool is_ok = false;
  do
    {
      translation_unit_sptr tu(new translation_unit(""));
      is_ok = read_translation_unit_from_input(ctxt, *tu);
      if (is_ok)
	corp.add(tu);
    }
  while (is_ok);

  return true;
}

/// Parse an ABI instrumentation file (in XML format) at a given path.
///
/// @param input_file a path to the file containing the xml document
/// to parse.
///
/// @param tu the translation unit resulting from the parsing.
///
/// @return true upon successful parsing, false otherwise.
bool
read_translation_unit_from_file(const string&		input_file,
				translation_unit&	tu)
{
  read_context read_ctxt(xml::new_reader_from_file(input_file));
  return read_translation_unit_from_input(read_ctxt, tu);
}

/// Parse an ABI instrumentation file (in XML format) at a given path.
/// The path used is the one associated to the instance of @ref
/// translation_unit.
///
/// @param tu the translation unit to populate with the de-serialized
/// from of what is read at translation_unit::get_path().
///
/// @return true upon successful parsing, false otherwise.
bool
read_translation_unit_from_file(translation_unit&	tu)
{return read_translation_unit_from_file(tu.get_path(), tu);}

/// Parse an ABI instrumentation file (in XML format) from an
/// in-memory buffer.
///
/// @param buffer the in-memory buffer containing the xml document to
/// parse.
///
/// @param tu the translation unit resulting from the parsing.
///
/// @return true upon successful parsing, false otherwise.
bool
read_translation_unit_from_buffer(const string&	buffer,
				  translation_unit&	tu)
{
  read_context read_ctxt(xml::new_reader_from_buffer(buffer));
  return read_translation_unit_from_input(read_ctxt, tu);
}

/// This function is called by @ref read_translation_unit_from_input.
/// It handles the current xml element node of the reading context.
/// The result of the "handling" is to build the representation of the
/// xml node and tied it to the current translation unit.
///
/// @param ctxt the current parsing context.
///
/// @return true upon successful completion, false otherwise.
static bool
handle_element(read_context&	ctxt)
{
  xml::reader_sptr reader = ctxt.get_reader();
  if (!reader)
    return false;

  if (xmlStrEqual (XML_READER_GET_NODE_NAME(reader).get(),
		   BAD_CAST("namespace-decl")))
    return handle_namespace_decl(ctxt);
  if (xmlStrEqual (XML_READER_GET_NODE_NAME(reader).get(),
		   BAD_CAST("type-decl")))
    return handle_type_decl(ctxt);
  if (xmlStrEqual (XML_READER_GET_NODE_NAME(reader).get(),
		   BAD_CAST("qualified-type-def")))
    return handle_qualified_type_decl(ctxt);
  if (xmlStrEqual (XML_READER_GET_NODE_NAME(reader).get(),
		   BAD_CAST("pointer-type-def")))
    return handle_pointer_type_def(ctxt);
  if (xmlStrEqual (XML_READER_GET_NODE_NAME(reader).get(),
		   BAD_CAST("reference-type-def")))
    return handle_reference_type_def(ctxt);
  if (xmlStrEqual (XML_READER_GET_NODE_NAME(reader).get(),
		   BAD_CAST("enum-decl")))
    return handle_enum_type_decl(ctxt);
  if (xmlStrEqual (XML_READER_GET_NODE_NAME(reader).get(),
		   BAD_CAST("typedef-decl")))
    return handle_typedef_decl(ctxt);
  if (xmlStrEqual (XML_READER_GET_NODE_NAME(reader).get(),
		   BAD_CAST("var-decl")))
    return handle_var_decl(ctxt);
  if (xmlStrEqual (XML_READER_GET_NODE_NAME(reader).get(),
		   BAD_CAST("function-decl")))
    return handle_function_decl(ctxt);
  if (xmlStrEqual (XML_READER_GET_NODE_NAME(reader).get(),
		   BAD_CAST("class-decl")))
    return handle_class_decl(ctxt);
  if (xmlStrEqual(XML_READER_GET_NODE_NAME(reader).get(),
		  BAD_CAST("function-template-decl")))
    return handle_function_tdecl(ctxt);
  if (xmlStrEqual(XML_READER_GET_NODE_NAME(reader).get(),
		  BAD_CAST("class-template-decl")))
    return handle_class_tdecl(ctxt);

  return false;
}

/// Parses location attributes on the current xml element node.
///
///@param ctxt the current parsing context
///
///@param loc the resulting location.
///
/// @return true upon sucessful parsing, false otherwise.
static bool
read_location(read_context& ctxt, location& loc)
{
  translation_unit& tu = *ctxt.get_translation_unit();

  xml::reader_sptr r = ctxt.get_reader();
  xml::xml_char_sptr f = XML_READER_GET_ATTRIBUTE(r, "filepath");
  if (!f)
    {
      loc = location();
      return true;
    }

  xml::xml_char_sptr l = XML_READER_GET_ATTRIBUTE(r, "line");
  xml::xml_char_sptr c = XML_READER_GET_ATTRIBUTE(r, "column");
  if (!l || !c)
    return false;

  loc = tu.get_loc_mgr().create_new_location
    (reinterpret_cast<char*>(f.get()),
     atoi(reinterpret_cast<char*>(l.get())),
     atoi(reinterpret_cast<char*>(c.get())));
  return true;
}

/// Parses location attributes on an xmlNodePtr.
///
///@param ctxt the current parsing context
///
///@param loc the resulting location.
///
/// @return true upon sucessful parsing, false otherwise.
static bool
read_location(read_context&	ctxt,
	      xmlNodePtr	node,
	      location&	loc)
{
  string file_path;
  size_t line = 0, column = 0;

  if (xml_char_sptr f = xml::build_sptr(xmlGetProp(node, BAD_CAST("filepath"))))
    file_path = CHAR_STR(f);

  if (file_path.empty())
    return false;

  if (xml_char_sptr l = xml::build_sptr(xmlGetProp(node, BAD_CAST("line"))))
    line = atoi(CHAR_STR(l));

  if (xml_char_sptr c = xml::build_sptr(xmlGetProp(node, BAD_CAST("column"))))
    column = atoi(CHAR_STR(c));

  loc =
    ctxt.get_translation_unit()->get_loc_mgr().create_new_location(file_path,
								   line,
								   column);
  return true;
}

/// Parse the visibility attribute.
///
/// @param node the xml node to read from.
///
/// @param vis the resulting visibility.
///
/// @return true upon successful completion, false otherwise.
static bool
read_visibility(xmlNodePtr node, decl_base::visibility& vis)
{
  if (xml_char_sptr s = XML_NODE_GET_ATTRIBUTE(node, "visibility"))
    {
      string v = CHAR_STR(s);

      if (v == "default")
	vis = decl_base::VISIBILITY_DEFAULT;
      else if (v == "hidden")
	vis = decl_base::VISIBILITY_HIDDEN;
      else if (v == "internal")
	vis = decl_base::VISIBILITY_INTERNAL;
      else if (v == "protected")
	vis = decl_base::VISIBILITY_PROTECTED;
      else
	vis = decl_base::VISIBILITY_DEFAULT;
      return true;
    }
  return false;
}

/// Parse the "binding" attribute on the current element.
///
/// @param node the xml node to build parse the bind from.
///
/// @param bind the resulting binding attribute.
///
/// @return true upon successful completion, false otherwise.
static bool
read_binding(xmlNodePtr node, decl_base::binding& bind)
{
  if (xml_char_sptr s = XML_NODE_GET_ATTRIBUTE(node, "binding"))
    {
      string b = CHAR_STR(s);

      if (b == "global")
	bind = decl_base::BINDING_GLOBAL;
      else if (b == "local")
	bind = decl_base::BINDING_LOCAL;
      else if (b == "weak")
	bind = decl_base::BINDING_WEAK;
      else
	bind = decl_base::BINDING_GLOBAL;
      return true;
    }

  return false;
}

/// Read the 'access' attribute on the current xml node.
///
/// @param node the xml node to consider.
///
/// @param access the access attribute.  Set iff the function returns true.
///
/// @return true upon sucessful completion, false otherwise.
static bool
read_access(xmlNodePtr node, class_decl::access_specifier& access)
{
  if (xml_char_sptr s = XML_NODE_GET_ATTRIBUTE(node, "access"))
    {
      string a = CHAR_STR(s);

      if (a == "private")
	access = class_decl::private_access;
      else if (a == "protected")
	access = class_decl::protected_access;
      else if (a == "public")
	access = class_decl::public_access;
      else
	access = class_decl::private_access;

      return true;
    }
  return false;
}

/// Parse 'size-in-bits' and 'alignment-in-bits' attributes of a given
/// xmlNodePtr reprensting an xml element.
///
/// @param node the xml element node to consider.
///
/// @param size_in_bits the resulting value for the 'size-in-bits'
/// attribute.  This set only if this function returns true and the if
/// the attribute was present on the xml element node.
///
/// @param align_in_bits the resulting value for the
/// 'alignment-in-bits' attribute.  This set only if this function
/// returns true and the if the attribute was present on the xml
/// element node.
///
/// @return true if either one of the two attributes above were set,
/// false otherwise.
static bool
read_size_and_alignment(xmlNodePtr node,
			size_t& size_in_bits,
			size_t& align_in_bits)
{

  bool got_something = false;
  if (xml_char_sptr s = XML_NODE_GET_ATTRIBUTE(node, "size-in-bits"))
    {
      size_in_bits = atoi(CHAR_STR(s));
      got_something = true;
    }

  if (xml_char_sptr s = XML_NODE_GET_ATTRIBUTE(node, "alignment-in-bits"))
    {
      align_in_bits = atoi(CHAR_STR(s));
      got_something = true;
    }
  return got_something;
}

/// Parse the 'static' attribute of a given xml element node.
///
/// @param node the xml element node to consider.
///
/// @param is_static the resulting the parsing.  Is set if the
/// function returns true.
///
/// @return true if the xml element node has the 'static' attribute
/// set, false otherwise.
static bool
read_static(xmlNodePtr node, bool& is_static)
{
  if (xml_char_sptr s = XML_NODE_GET_ATTRIBUTE(node, "static"))
    {
      string b = CHAR_STR(s);
      is_static = (b == "yes") ? true : false;
      return true;
    }
  return false;
}

/// Parse the 'layout-offset-in-bits' attribute of a given xml element node.
///
/// @param offset_in_bits set to true if the element node contains the
/// attribute.
///
/// @return true iff the xml element node contain$s the attribute.
static bool
read_offset_in_bits(xmlNodePtr	node,
		    size_t&	offset_in_bits)
{
  if (xml_char_sptr s = XML_NODE_GET_ATTRIBUTE(node, "layout-offset-in-bits"))
    {
      offset_in_bits = atoi(CHAR_STR(s));
      return true;
    }
  return false;
}

/// Parse the 'constructor', 'destructor' and 'const' attribute of a
/// given xml node.
///
/// @param is_constructor the resulting value of the parsing of the
/// 'constructor' attribute.  Is set if the xml node contains the
/// attribute and if the function returns true.
///
/// @param is_destructor the resulting value of the parsing of the
/// 'destructor' attribute.  Is set if the xml node contains the
/// attribute and if the function returns true.
///
/// @param is_const the resulting value of the parsing of the 'const'
/// attribute.  Is set if the xml node contains the attribute and if
/// the function returns true.
///
/// @return true if at least of the attributes above is set, false
/// otherwise.
///
/// Note that callers of this function should initialize
/// is_constructor, is_destructor and is_const prior to passing them
/// to this function.
static bool
read_cdtor_const(xmlNodePtr	node,
		 bool&		is_constructor,
		 bool&		is_destructor,
		 bool&		is_const)
{
  if (xml_char_sptr s = XML_NODE_GET_ATTRIBUTE(node, "constructor"))
    {
      string b = CHAR_STR(s);
      if (b == "yes")
	is_constructor = true;
      else
	is_constructor = false;

      return true;
    }

  if (xml_char_sptr s = XML_NODE_GET_ATTRIBUTE(node, "destructor"))
    {
      string b = CHAR_STR(s);
      if (b == "yes")
	is_destructor = true;
      else
	is_destructor = false;

      return true;
    }

  if (xml_char_sptr s = XML_NODE_GET_ATTRIBUTE(node, "const"))
    {
      string b = CHAR_STR(s);
      if (b == "yes")
	is_const = true;
      else
	is_const = false;

      return true;
    }

  return false;
}

/// Read the "is-declaration-only" attribute of the current xml node.
///
/// @param node the xml node to consider.
///
/// @param is_decl_only is set to true iff the "is-declaration-only" attribute
/// is present and set to "yes"
///
/// @return true iff the is_decl_only attribute was set.
static bool
read_is_declaration_only(xmlNodePtr node, bool& is_decl_only)
{
    if (xml_char_sptr s = XML_NODE_GET_ATTRIBUTE(node, "is-declaration-only"))
      {
	string str = CHAR_STR(s);
	if (str == "yes")
	  is_decl_only = true;
	else
	  is_decl_only = false;
	return true;
      }
    return false;
}

/// Read the "is-virtual" attribute of the current xml node.
///
/// @param node the xml node to read the attribute from
///
/// @param is_virtual is set to true iff the "is-virtual" attribute is
/// present and set to "yes".
///
/// @return true iff the is-virtual attribute is present.
static bool
read_is_virtual(xmlNodePtr node, bool& is_virtual)
{
  if (xml_char_sptr s = XML_NODE_GET_ATTRIBUTE(node, "is-virtual"))
    {
      string str = CHAR_STR(s);
      if (str == "yes")
	is_virtual = true;
      else
	is_virtual = false;
      return true;
    }
  return false;
}

/// Build a function parameter from a 'parameter' xml element node.
///
/// @param ctxt the contexte of the xml parsing.
///
/// @param node the xml 'parameter' element node to de-serialize from.
static shared_ptr<function_decl::parameter>
build_function_parameter(read_context& ctxt, const xmlNodePtr node)
{
  shared_ptr<function_decl::parameter> nil;

  if (!xmlStrEqual(node->name, BAD_CAST("parameter")))
    return nil;

  bool is_variadic = false;
  string is_variadic_str;
  if (xml_char_sptr s =
      xml::build_sptr(xmlGetProp(node, BAD_CAST("is-variadic"))))
    {
      is_variadic_str = CHAR_STR(s) ? CHAR_STR(s) : "";
      is_variadic = (is_variadic_str == "yes") ? true : false;
    }

  bool is_artificial = false;
  string is_artificial_str;
  if (xml_char_sptr s =
      xml::build_sptr(xmlGetProp(node, BAD_CAST("is-artificial"))))
    {
      is_artificial_str = CHAR_STR(s) ? CHAR_STR(s) : "";
      is_artificial = (is_artificial_str == "yes") ? true : false;
    }

  string type_id;
  if (xml_char_sptr a = xml::build_sptr(xmlGetProp(node, BAD_CAST("type-id"))))
    type_id = CHAR_STR(a);

  shared_ptr<type_base> type = ctxt.get_type_decl(type_id);
  assert(type || is_variadic);

  string name;
  if (xml_char_sptr a = xml::build_sptr(xmlGetProp(node, BAD_CAST("name"))))
    name = CHAR_STR(a);

  location loc;
  read_location(ctxt, node, loc);

  shared_ptr<function_decl::parameter> p
    (new function_decl::parameter(type, name, loc, is_variadic, is_artificial));

  return p;
}

/// Build a function_decl from a 'function-decl' xml node.
///
/// @param ctxt the context of the parsing.
///
/// @param node the xml node to build the function_decl from.
///
/// @param as_method_decl if this is set to a class_decl pointer, it
/// means that the 'function-decl' xml node should be parsed as a
/// method_decl.  The class_decl pointer is the class decl to which
/// the resulting method_decl is a member function of.  The resulting
/// shared_ptr<function_decl> that is returned is then really a
/// shared_ptr<class_decl::method_decl>.
///
/// @param update_depth_info should be set to true if the function
/// should update the depth information maintained in the parsing
/// context.  If the xml element node has been 'hit' by the
/// advence_cursor then this should be set to false, because that
/// function updates the depth information maintained in the parsing
/// context already.
///
/// @return a pointer to a newly created function_decl upon successful
/// completion, a null pointer otherwise.
static shared_ptr<function_decl>
build_function_decl(read_context&	ctxt,
		    const xmlNodePtr	node,
		    shared_ptr<class_decl> as_method_decl,
		    bool update_depth_info,
		    bool add_to_current_scope)
{
  shared_ptr<function_decl> nil;

  if (!xmlStrEqual(node->name, BAD_CAST("function-decl")))
    return nil;

  string name;
  if (xml_char_sptr s = XML_NODE_GET_ATTRIBUTE(node, "name"))
    name = xml::unescape_xml_string(CHAR_STR(s));

  string mangled_name;
  if (xml_char_sptr s = XML_NODE_GET_ATTRIBUTE(node, "mangled-name"))
    mangled_name = xml::unescape_xml_string(CHAR_STR(s));

  string inline_prop;
  if (xml_char_sptr s = XML_NODE_GET_ATTRIBUTE(node, "declared-inline"))
    inline_prop = CHAR_STR(s);
  bool declared_inline = inline_prop == "yes" ? true : false;

  decl_base::visibility vis = decl_base::VISIBILITY_NONE;
  read_visibility(node, vis);

  decl_base::binding bind = decl_base::BINDING_NONE;
  read_binding(node, bind);

  size_t size = 0, align = 0;
  read_size_and_alignment(node, size, align);

  location loc;
  read_location(ctxt, node, loc);

  std::vector<shared_ptr<function_decl::parameter> > parms;
  shared_ptr<function_type> fn_type(as_method_decl
				    ? new method_type(as_method_decl,
						      size, align)
				    : new function_type(size, align));

  shared_ptr<function_decl> fn_decl(as_method_decl
				    ? new class_decl::method_decl
				    (name, fn_type,
				     declared_inline, loc,
				     mangled_name, vis, bind)
				    : new function_decl(name, fn_type,
							declared_inline, loc,
							mangled_name, vis,
							bind));

  ctxt.push_decl_to_current_scope(fn_decl, node, update_depth_info,
				  add_to_current_scope);

  for (xmlNodePtr n = node->children; n ; n = n->next)
    {
      if (n->type != XML_ELEMENT_NODE)
	continue;

      if (xmlStrEqual(n->name, BAD_CAST("parameter")))
	{
	  if (shared_ptr<function_decl::parameter> p =
	      build_function_parameter(ctxt, n))
	    fn_type->append_parameter(p);
	}
      else if (xmlStrEqual(n->name, BAD_CAST("return")))
	{
	  string type_id;
	  if (xml_char_sptr s =
	      xml::build_sptr(xmlGetProp(n, BAD_CAST("type-id"))))
	    type_id = CHAR_STR(s);
	  if (!type_id.empty())
	    fn_type->set_return_type(ctxt.get_type_decl(type_id));
	}
    }

  return fn_decl;
}

/// Build pointer to var_decl from a 'var-decl' xml Node
///
/// @param ctxt the context of the parsing.
///
/// @param node the xml node to build the var_decl from.
///
/// @return a pointer to a newly built var_decl upon successful
/// completion, a null pointer otherwise.
static shared_ptr<var_decl>
build_var_decl(read_context& ctxt, const xmlNodePtr node,
	       bool update_depth_info, bool add_to_current_scope)
{
  shared_ptr<var_decl> nil;

  if (!xmlStrEqual(node->name, BAD_CAST("var-decl")))
    return nil;

  string name;
  if (xml_char_sptr s = XML_NODE_GET_ATTRIBUTE(node, "name"))
    name = xml::unescape_xml_string(CHAR_STR(s));

  string type_id;
  if (xml_char_sptr s = XML_NODE_GET_ATTRIBUTE(node, "type-id"))
    type_id = CHAR_STR(s);
  shared_ptr<type_base> underlying_type = ctxt.get_type_decl(type_id);
  assert(underlying_type);

  string mangled_name;
  if (xml_char_sptr s = XML_NODE_GET_ATTRIBUTE(node, "mangled-name"))
    mangled_name = xml::unescape_xml_string(CHAR_STR(s));

  decl_base::visibility vis = decl_base::VISIBILITY_NONE;
  read_visibility(node, vis);

  decl_base::binding bind = decl_base::BINDING_NONE;
  read_binding(node, bind);

  location locus;
  read_location(ctxt, node, locus);

  shared_ptr<var_decl> decl(new var_decl(name, underlying_type,
					 locus, mangled_name,
					 vis, bind));

  ctxt.push_decl_to_current_scope(decl, node, update_depth_info,
				  add_to_current_scope);

  return decl;
}

/// Build a type_decl from a "type-decl" XML Node.
///
/// @param ctxt the context of the parsing.
///
/// @param node the XML node to build the type_decl from.
///
/// @return a pointer to type_decl upon successful completion, a null
/// pointer otherwise.
static shared_ptr<type_decl>
build_type_decl(read_context&		ctxt,
		const xmlNodePtr	node,
		bool update_depth_info,
		bool add_to_current_scope)
{
  shared_ptr<type_decl> nil;

  if (!xmlStrEqual(node->name, BAD_CAST("type-decl")))
    return shared_ptr<type_decl>((type_decl*)0);

  string name;
  if (xml_char_sptr s = XML_NODE_GET_ATTRIBUTE(node, "name"))
    name = xml::unescape_xml_string(CHAR_STR(s));

  string id;
  if (xml_char_sptr s = XML_NODE_GET_ATTRIBUTE(node, "id"))
    id = CHAR_STR(s);

  size_t size_in_bits= 0;
  if (xml_char_sptr s = XML_NODE_GET_ATTRIBUTE(node, "size-in-bits"))
    size_in_bits = atoi(CHAR_STR(s));

  size_t alignment_in_bits = 0;
  if (xml_char_sptr s = XML_NODE_GET_ATTRIBUTE(node, "alignment-in-bits"))
    alignment_in_bits = atoi(CHAR_STR(s));

  location loc;
  read_location(ctxt, node, loc);

  assert (!ctxt.get_type_decl(id));

  shared_ptr<type_decl> decl(new type_decl(name, size_in_bits,
					   alignment_in_bits,
					   loc));
  if (ctxt.push_and_key_type_decl(decl, id, node, update_depth_info,
				  add_to_current_scope))
    return decl;

  return nil;
}

/// Build a qualified_type_def from a 'qualified-type-def' xml node.
///
/// @param ctxt the context of the parsing.
///
/// @param node the xml node to build the qualified_type_def from.
///
/// @return a pointer to a newly built qualified_type_def upon
/// successful completion, a null pointer otherwise.
static shared_ptr<qualified_type_def>
build_qualified_type_decl(read_context& ctxt,
			  const xmlNodePtr node,
			  bool update_depth_info,
			  bool add_to_current_scope)
{
  if (!xmlStrEqual(node->name, BAD_CAST("qualified-type-def")))
    return shared_ptr<qualified_type_def>((qualified_type_def*)0);

  string type_id;
  if (xml_char_sptr s = XML_NODE_GET_ATTRIBUTE(node, "type-id"))
    type_id = CHAR_STR(s);

  shared_ptr<type_base> underlying_type = ctxt.get_type_decl(type_id);
  assert(underlying_type);

  string id;
  if (xml_char_sptr s = XML_NODE_GET_ATTRIBUTE (node, "id"))
    id = CHAR_STR(s);

  assert(!id.empty() && !ctxt.get_type_decl(id));

  string const_str;
  if (xml_char_sptr s = XML_NODE_GET_ATTRIBUTE(node, "const"))
    const_str = CHAR_STR(s);
  bool const_cv = const_str == "yes" ? true : false;

  string volatile_str;
  if (xml_char_sptr s = XML_NODE_GET_ATTRIBUTE(node, "volatile"))
    volatile_str = CHAR_STR(s);
  bool volatile_cv = volatile_str == "yes" ? true : false;

  qualified_type_def::CV cv = qualified_type_def::CV_NONE;
  if (const_cv)
    cv = cv | qualified_type_def::CV_CONST;
  if (volatile_cv)
    cv = cv | qualified_type_def::CV_VOLATILE;

  location loc;
  read_location(ctxt, node, loc);

  shared_ptr<qualified_type_def> decl(new qualified_type_def(underlying_type,
							     cv, loc));
  if (ctxt.push_and_key_type_decl(decl, id, node, update_depth_info,
				  add_to_current_scope))
    return decl;

  return shared_ptr<qualified_type_def>((qualified_type_def*)0);
}

/// Build a pointer_type_def from a 'pointer-type-def' xml node.
///
/// @param ctxt the context of the parsing.
///
/// @param node the xml node to build the pointer_type_def from.
///
/// @return a pointer to a newly built pointer_type_def upon
/// successful completion, a null pointer otherwise.
static shared_ptr<pointer_type_def>
build_pointer_type_def(read_context&	ctxt,
		       const xmlNodePtr node,
		       bool update_depth_info,
		       bool add_to_current_scope)
{

  shared_ptr<pointer_type_def> nil;

  if (!xmlStrEqual(node->name, BAD_CAST("pointer-type-def")))
    return nil;

  string type_id;
  if (xml_char_sptr s = XML_NODE_GET_ATTRIBUTE(node, "type-id"))
    type_id = CHAR_STR(s);

  shared_ptr<type_base> pointed_to_type = ctxt.get_type_decl(type_id);
  assert(pointed_to_type);

  size_t size_in_bits = 0, alignment_in_bits = 0;
  if (xml_char_sptr s = XML_NODE_GET_ATTRIBUTE(node, "size-in-bits"))
    size_in_bits = atoi(CHAR_STR(s));
  if (xml_char_sptr s = XML_NODE_GET_ATTRIBUTE(node, "alignment-in-bits"))
    alignment_in_bits = atoi(CHAR_STR(s));

  string id;
  if (xml_char_sptr s = XML_NODE_GET_ATTRIBUTE(node, "id"))
    id = CHAR_STR(s);
  assert(!id.empty() && !ctxt.get_type_decl(id));

  location loc;
  read_location(ctxt, node, loc);

  shared_ptr<pointer_type_def> t(new pointer_type_def(pointed_to_type,
						      size_in_bits,
						      alignment_in_bits,
						      loc));
  if (ctxt.push_and_key_type_decl(t, id, node, update_depth_info,
				  add_to_current_scope))
    return t;

  return nil;
}

/// Build a reference_type_def from a pointer to 'reference-type-def'
/// xml node.
///
/// @param ctxt the context of the parsing.
///
/// @param node the xml node to build the reference_type_def from.
///
/// @return a pointer to a newly built reference_type_def upon
/// successful completio, a null pointer otherwise.
static shared_ptr<reference_type_def>
build_reference_type_def(read_context&	ctxt,
			 const xmlNodePtr node, bool update_depth_info,
			 bool add_to_current_scope)
{
  shared_ptr<reference_type_def> nil;

  if (!xmlStrEqual(node->name, BAD_CAST("reference-type-def")))
    return nil;

  string kind;
  if (xml_char_sptr s = XML_NODE_GET_ATTRIBUTE(node, "kind"))
    kind = CHAR_STR(s); // this should be either "lvalue" or "rvalue".
  bool is_lvalue = kind == "lvalue" ? true : false;

  string type_id;
  if (xml_char_sptr s = XML_NODE_GET_ATTRIBUTE(node, "type-id"))
    type_id = CHAR_STR(s);

  shared_ptr<type_base> pointed_to_type = ctxt.get_type_decl(type_id);
  assert(pointed_to_type);

  size_t size_in_bits = 0, alignment_in_bits = 0;
  if (xml_char_sptr s = XML_NODE_GET_ATTRIBUTE(node, "size-in-bits"))
    size_in_bits = atoi(CHAR_STR(s));
  if (xml_char_sptr s = XML_NODE_GET_ATTRIBUTE(node, "alignment-in-bits"))
    alignment_in_bits = atoi(CHAR_STR(s));

  string id;
  if (xml_char_sptr s = XML_NODE_GET_ATTRIBUTE(node, "id"))
    id = CHAR_STR(s);
  assert(!id.empty() && !ctxt.get_type_decl(id));

  location loc;
  read_location(ctxt, node, loc);

  shared_ptr<reference_type_def> t(new reference_type_def(pointed_to_type,
							  is_lvalue,
							  size_in_bits,
							  alignment_in_bits,
							  loc));
  if (ctxt.push_and_key_type_decl(t, id, node, update_depth_info,
				  add_to_current_scope))
    return t;

  return nil;
}

/// Build an enum_type_decl from an 'enum-type-decl' xml node.
///
/// @param ctxt the context of the parsing.
///
/// @param node the xml node to build the enum_type_decl from.
///
/// @return a pointer to a newly built enum_type_decl upon successful
/// completion, a null pointer otherwise.
static shared_ptr<enum_type_decl>
build_enum_type_decl(read_context&  ctxt, const xmlNodePtr node,
		     bool update_depth_info, bool add_to_current_scope)
{
  shared_ptr<enum_type_decl> nil;

  if (!xmlStrEqual(node->name, BAD_CAST("enum-decl")))
    return nil;

  string name;
  if (xml_char_sptr s = XML_NODE_GET_ATTRIBUTE(node, "name"))
    name = xml::unescape_xml_string(CHAR_STR(s));

  location loc;
  read_location(ctxt, node, loc);

  string id;
  if (xml_char_sptr s = XML_NODE_GET_ATTRIBUTE(node, "id"))
    id = CHAR_STR(s);

  assert(!id.empty() && !ctxt.get_type_decl(id));

  string base_type_id;
  enum_type_decl::enumerators enums;
  for (xmlNodePtr n = node->children; n; n = n->next)
    {
      if (n->type != XML_ELEMENT_NODE)
	continue;

      if (xmlStrEqual(n->name, BAD_CAST("underlying-type")))
	{
	  xml_char_sptr a = xml::build_sptr(xmlGetProp(n, BAD_CAST("type-id")));
	  if (a)
	    base_type_id = CHAR_STR(a);
	  continue;
	}

      if (xmlStrEqual(n->name, BAD_CAST("enumerator")))
	{
	  string name;
	  size_t value = 0;

	  xml_char_sptr a = xml::build_sptr(xmlGetProp(n, BAD_CAST("name")));
	  if (a)
	    name = xml::unescape_xml_string(CHAR_STR(a));

	  a = xml::build_sptr(xmlGetProp(n, BAD_CAST("value")));
	  if (a)
	    value = atoi(CHAR_STR(a));

	  enums.push_back(enum_type_decl::enumerator(name, value));
	}
    }

  shared_ptr<type_base> underlying_type = ctxt.get_type_decl(base_type_id);
  assert(underlying_type);

  shared_ptr<enum_type_decl> t(new enum_type_decl(name, loc,
						  underlying_type,
						  enums));
  if (ctxt.push_and_key_type_decl(t, id, node, update_depth_info,
				  add_to_current_scope))
    return t;

  return nil;
}

/// Build a typedef_decl from a 'typedef-decl' xml node.
///
/// @param ctxt the context of the parsing.
///
/// @param node the xml node to build the typedef_decl from.
///
/// @return a pointer to a newly built typedef_decl upon successful
/// completion, a null pointer otherwise.
static shared_ptr<typedef_decl>
build_typedef_decl(read_context&	ctxt,
		   const xmlNodePtr	node,
		   bool update_depth_info,
		   bool add_to_current_scope)
{
  shared_ptr<typedef_decl> nil;

  if (!xmlStrEqual(node->name, BAD_CAST("typedef-decl")))
    return nil;

  string name;
  if (xml_char_sptr s = XML_NODE_GET_ATTRIBUTE(node, "name"))
    name = xml::unescape_xml_string(CHAR_STR(s));

  string type_id;
  if (xml_char_sptr s = XML_NODE_GET_ATTRIBUTE(node, "type-id"))
    type_id = CHAR_STR(s);
  shared_ptr<type_base> underlying_type(ctxt.get_type_decl(type_id));
  assert(underlying_type);

  string id;
  if (xml_char_sptr s = XML_NODE_GET_ATTRIBUTE(node, "id"))
    id = CHAR_STR(s);
  assert(!id.empty() && !ctxt.get_type_decl(id));

  location loc;
  read_location(ctxt, node, loc);

  shared_ptr<typedef_decl> t(new typedef_decl(name, underlying_type, loc));

  if (ctxt.push_and_key_type_decl(t, id, node, update_depth_info,
				  add_to_current_scope))
    return t;

  return nil;
}

/// Build a class_decl from a 'class-decl' xml node.
///
/// @param ctxt the context of the parsing.
///
/// @param node the xml node to build the class_decl from.
///
/// @param update_depth_info whether to update the depth info carried
/// by the parsing context.
///
/// @return a pointer to class_decl upon successful completion, a null
/// pointer otherwise.
static shared_ptr<class_decl>
build_class_decl(read_context& ctxt, const xmlNodePtr node,
		 bool update_depth_info, bool add_to_current_scope)
{
  shared_ptr<class_decl> nil;

  if (!xmlStrEqual(node->name, BAD_CAST("class-decl")))
    return nil;

  string name;
  if (xml_char_sptr s = XML_NODE_GET_ATTRIBUTE(node, "name"))
    name = xml::unescape_xml_string(CHAR_STR(s));

  size_t size_in_bits = 0, alignment_in_bits = 0;
  read_size_and_alignment(node, size_in_bits, alignment_in_bits);

  decl_base::visibility vis = decl_base::VISIBILITY_NONE;
  read_visibility(node, vis);

  string id;
  if (xml_char_sptr s = XML_NODE_GET_ATTRIBUTE(node, "id"))
    id = CHAR_STR(s);

  // If the id is not empty, then we should be seeing this type for
  // the first time, unless it's a declaration-only type class.
  if (!id.empty())
    {
      type_base_sptr t = ctxt.get_type_decl(id);
      if (t)
	{
	  class_decl_sptr c = dynamic_pointer_cast<class_decl>(t);
	  assert(c && c->is_declaration_only());
	}
    }

  location loc;
  read_location(ctxt, node, loc);

  class_decl::member_types mbrs;
  class_decl::data_members data_mbrs;
  class_decl::member_functions mbr_functions;
  class_decl::base_specs  bases;

  shared_ptr<class_decl> decl;

  bool is_decl_only = false;
  read_is_declaration_only(node, is_decl_only);

  if (!is_decl_only)
    decl.reset(new class_decl(name, size_in_bits, alignment_in_bits,
			      loc, vis, bases, mbrs, data_mbrs,
			      mbr_functions));

  string def_id;
  bool is_def_of_decl = false;
  if (xml_char_sptr s = XML_NODE_GET_ATTRIBUTE(node, "def-of-decl-id"))
    def_id = CHAR_STR(s);

  if (!def_id.empty())
    {
      shared_ptr<class_decl> d =
	dynamic_pointer_cast<class_decl>(ctxt.get_type_decl(def_id));
      if (d && d->is_declaration_only())
	{
	  is_def_of_decl = true;
	  decl->set_earlier_declaration(d);
	}
    }

  assert(!is_decl_only || !is_def_of_decl);

  if (is_decl_only)
    decl.reset(new class_decl(name));

  ctxt.push_decl_to_current_scope(decl, node, update_depth_info,
				  add_to_current_scope);

  for (xmlNodePtr n = node->children; !is_decl_only && n; n = n->next)
    {
      if (n->type != XML_ELEMENT_NODE)
	continue;

      if (xmlStrEqual(n->name, BAD_CAST("base-class")))
	{
	  class_decl::access_specifier access = class_decl::private_access;
	  read_access(n, access);

	  string type_id;
	  if (xml_char_sptr s = XML_NODE_GET_ATTRIBUTE(n, "type-id"))
	    type_id = CHAR_STR(s);
	  shared_ptr<class_decl> b =
	    dynamic_pointer_cast<class_decl>(ctxt.get_type_decl(type_id));
	  assert(b);

	  size_t offset_in_bits = 0;
	  bool offset_present = read_offset_in_bits (n, offset_in_bits);

	  bool is_virtual = false;
	  read_is_virtual (n, is_virtual);

	  shared_ptr<class_decl::base_spec> base (new class_decl::base_spec
						  (b, access,
						   offset_present
						   ? (long) offset_in_bits
						   : -1,
						   is_virtual));
	  decl->add_base_specifier(base);
	}
      else if (xmlStrEqual(n->name, BAD_CAST("member-type")))
	{
	  class_decl::access_specifier access = class_decl::private_access;
	  read_access(n, access);

	  for (xmlNodePtr p = n->children; p; p = p->next)
	    {
	      if (p->type != XML_ELEMENT_NODE)
		continue;

	      if (shared_ptr<type_base> t =
		  build_type(ctxt, p, /*update_depth_info=*/true,
			     /*add_to_current_scope=*/true))
		{
		  // No need to add the type to the class as
		  //build_type() above has just done that.
		  //decl->add_member_type(t, access)
		  ;
		}
	    }
	}
      else if (xmlStrEqual(n->name, BAD_CAST("data-member")))
	{
	  class_decl::access_specifier access = class_decl::private_access;
	  read_access(n, access);

	  bool is_laid_out = false;
	  size_t offset_in_bits = 0;
	  if (read_offset_in_bits(n, offset_in_bits))
	    is_laid_out = true;

	  bool is_static = false;
	  read_static(n, is_static);

	  for (xmlNodePtr p = n->children; p; p = p->next)
	    {
	      if (p->type != XML_ELEMENT_NODE)
		continue;

	      if (shared_ptr<var_decl> v =
		  build_var_decl(ctxt, p, /*update_depth_info=*/true,
				 /*add_to_current_scope=*/false))
		decl->add_data_member(v, access, is_laid_out,
				      is_static, offset_in_bits);
	    }
	}
      else if (xmlStrEqual(n->name, BAD_CAST("member-function")))
	{
	  class_decl::access_specifier access = class_decl::private_access;
	  read_access(n, access);

	  size_t vtable_offset = 0;
	  if (xml_char_sptr s =
	      XML_NODE_GET_ATTRIBUTE(n, "vtable-offset"))
	    vtable_offset = atoi(CHAR_STR(s));

	  bool is_static = false;
	  read_static(n, is_static);

	  bool is_ctor = false, is_dtor = false, is_const = false;
	  read_cdtor_const(n, is_ctor, is_dtor, is_const);

	  for (xmlNodePtr p = n->children; p; p = p->next)
	    {
	      if (p->type != XML_ELEMENT_NODE)
		continue;

	      if (shared_ptr<function_decl> f =
		  build_function_decl(ctxt, p, decl,
				      /*update_depth_info=*/true,
				      /*add_to_current_scope=*/false))
		decl->add_member_function(f, access,
					  vtable_offset,
					  is_static,
					  is_ctor, is_dtor,
					  is_const);
	    }
	}
      else if (xmlStrEqual(n->name, BAD_CAST("member-template")))
	{
	  class_decl::access_specifier access = class_decl::private_access;
	  read_access(n, access);

	  bool is_static = false;
	  read_static(n, is_static);

	  bool is_ctor = false, is_dtor = false, is_const = false;
	  read_cdtor_const(n, is_ctor, is_dtor, is_const);

	  for (xmlNodePtr p = n->children; p; p = p->next)
	    {
	      if (p->type != XML_ELEMENT_NODE)
		continue;

	      if (shared_ptr<function_tdecl> f =
		  build_function_tdecl(ctxt, p,
				       /*update_depth_info=*/true,
				       /*add_to_current_scope=*/false))
		{
		  shared_ptr<class_decl::member_function_template> m
		    (new class_decl::member_function_template(f, access,
							      is_static,
							      is_ctor,
							      is_const));
		  assert(!f->get_scope());
		  decl->add_member_function_template(m);
		}
	      else if (shared_ptr<class_tdecl> c =
		       build_class_tdecl(ctxt, p,
					 /*update_depth_info=*/true,
					 /*add_to_current_scope=*/false))
		{
		  shared_ptr<class_decl::member_class_template> m
		    (new class_decl::member_class_template(c, access,
							   is_static));
		  assert(!c->get_scope());
		  decl->add_member_class_template(m);
		}
	    }
	}
    }

  if (decl)
    ctxt.key_type_decl(decl, id);

  return decl;
}

/// Build an intance of function_tdecl, from an
/// 'function-template-decl' xml element node.
///
/// @param ctxt the context of the parsing.
///
/// @param node the xml node to parse from.
///
/// @param update_depth_info this must be set to false, if we reached
/// this xml node by calling the xmlTextReaderRead function.  In that
/// case, build_function_decl doesn't have to update the depth
/// information that is maintained in the context of the parsing.
/// Otherwise if this node if just a child grand child of a node that
/// we reached using xmlTextReaderRead, of if it wasn't reached via
/// xmlTextReaderRead at all,then the argument to this parameter
/// should be true.  In that case this function will update the depth
/// information that is maintained by in the context of the parsing.
///
/// @return the newly built function_tdecl upon successful
/// completion, a null pointer otherwise.
static shared_ptr<function_tdecl>
build_function_tdecl(read_context& ctxt,
		     const xmlNodePtr node,
		     bool update_depth_info,
		     bool add_to_current_scope)
{
  shared_ptr<function_tdecl> nil, result;

  if (!xmlStrEqual(node->name, BAD_CAST("function-template-decl")))
    return nil;

  string id;
  if (xml_char_sptr s = XML_NODE_GET_ATTRIBUTE(node, "id"))
    id = CHAR_STR(s);
  if (id.empty() || ctxt.get_fn_tmpl_decl(id))
    return nil;

  location loc;
  read_location(ctxt, node, loc);

  decl_base::visibility vis = decl_base::VISIBILITY_NONE;
  read_visibility(node, vis);

  decl_base::binding bind = decl_base::BINDING_NONE;
  read_binding(node, bind);

  shared_ptr<function_tdecl> fn_tmpl_decl
    (new function_tdecl(loc, vis, bind));

  ctxt.push_decl_to_current_scope(fn_tmpl_decl, node,
				  update_depth_info,
				  add_to_current_scope);

  unsigned parm_index = 0;
  for (xmlNodePtr n = node->children; n; n = n->next)
    {
      if (n->type != XML_ELEMENT_NODE)
	continue;

      if (shared_ptr<template_parameter> parm =
	  build_template_parameter(ctxt, n, parm_index,
				   /*update_depth_info=*/true))
	{
	  fn_tmpl_decl->add_template_parameter(parm);
	  ++parm_index;
	}
      else if (shared_ptr<function_decl> f =
	       build_function_decl(ctxt, n, shared_ptr<class_decl>(),
				   /*update_depth_info=*/true,
				   /*add_to_current_scope=*/true))
	fn_tmpl_decl->set_pattern(f);
    }

  ctxt.key_fn_tmpl_decl(fn_tmpl_decl, id);

  return fn_tmpl_decl;
}

/// Build an intance of class_tdecl, from a
/// 'class-template-decl' xml element node.
///
/// @param ctxt the context of the parsing.
///
/// @param node the xml node to parse from.
///
/// @param update_depth_info this must be set to false, if we reached
/// this xml node by calling the xmlTextReaderRead function.  In that
/// case, build_class_decl doesn't have to update the depth
/// information that is maintained in the context of the parsing.
/// Otherwise if this node if just a child grand child of a node that
/// we reached using xmlTextReaderRead, of if it wasn't reached via
/// xmlTextReaderRead at all,then the argument to this parameter
/// should be true.  In that case this function will update the depth
/// information that is maintained by in the context of the parsing.
///
/// @return the newly built function_tdecl upon successful
/// completion, a null pointer otherwise.
static shared_ptr<class_tdecl>
build_class_tdecl(read_context&	ctxt, const xmlNodePtr node,
		  bool update_depth_info, bool add_to_current_scope)
{
  shared_ptr<class_tdecl> nil, result;

  if (!xmlStrEqual(node->name, BAD_CAST("class-template-decl")))
    return nil;

  string id;
  if (xml_char_sptr s = XML_NODE_GET_ATTRIBUTE(node, "id"))
    id = CHAR_STR(s);
  if (id.empty() || ctxt.get_class_tmpl_decl(id))
    return nil;

  location loc;
  read_location(ctxt, node, loc);

  decl_base::visibility vis = decl_base::VISIBILITY_NONE;
  read_visibility(node, vis);

  shared_ptr<class_tdecl> class_tmpl
    (new class_tdecl(loc, vis));

  ctxt.push_decl_to_current_scope(class_tmpl, node, update_depth_info,
				  add_to_current_scope);

  unsigned parm_index = 0;
  for (xmlNodePtr n = node->children; n; n = n->next)
    {
      if (n->type != XML_ELEMENT_NODE)
	continue;

      if (shared_ptr<template_parameter> parm=
	  build_template_parameter(ctxt, n, parm_index,
				   /*update_depth_info=*/true))
	{
	  class_tmpl->add_template_parameter(parm);
	  ++parm_index;
	}
      else if (shared_ptr<class_decl> c =
	       build_class_decl(ctxt, n, /*update_depth_info=*/true,
				add_to_current_scope))
	class_tmpl->set_pattern(c);
    }

  ctxt.key_class_tmpl_decl(class_tmpl, id);

  return class_tmpl;
}

/// Build a type_tparameter from a 'template-type-parameter'
/// xml element node.
///
/// @param ctxt the context of the parsing.
///
/// @param node the xml node to parse from.
///
/// @param index the index (occurrence index, starting from 0) of the
/// template parameter.
///
/// @return a pointer to a newly created instance of
/// type_tparameter, a null pointer otherwise.
static shared_ptr<type_tparameter>
build_type_tparameter(read_context& ctxt, const xmlNodePtr node,
		      unsigned index, bool update_depth_info)
{
  shared_ptr<type_tparameter> nil, result;

  if (!xmlStrEqual(node->name, BAD_CAST("template-type-parameter")))
    return nil;

  string id;
  if (xml_char_sptr s = XML_NODE_GET_ATTRIBUTE(node, "id"))
    id = CHAR_STR(s);
  if (!id.empty())
    assert(!ctxt.get_type_decl(id));

  string type_id;
  if (xml_char_sptr s = XML_NODE_GET_ATTRIBUTE(node, "type-id"))
    type_id = CHAR_STR(s);
  if (!type_id.empty()
      && !(result = dynamic_pointer_cast<type_tparameter>
	   (ctxt.get_type_decl(type_id))))
    abort();

  string name;
  if (xml_char_sptr s = XML_NODE_GET_ATTRIBUTE(node, "name"))
    name = xml::unescape_xml_string(CHAR_STR(s));

  location loc;
  read_location(ctxt, node,loc);

  result.reset(new type_tparameter(index, name, loc));

  if (id.empty())
    ctxt.push_decl_to_current_scope(dynamic_pointer_cast<decl_base>(result),
				    node, update_depth_info,
				    /*add_to_current_scope=*/true);
  else
    ctxt.push_and_key_type_decl(result, id, node, update_depth_info,
				/*add_to_current_scope=*/true);

  return result;
}


/// Build a tmpl_parm_type_composition from a
/// "template-parameter-type-composition" xml element node.
///
/// @param ctxt the context of the parsing.
///
/// @param node the xml node to parse from.
///
/// @param index the index of the previous normal template parameter.
///
/// @param update_depth_info wheter to udpate the depth information
/// maintained in the context of the parsing.
///
/// @return a pointer to a new instance of tmpl_parm_type_composition
/// upon successful completion, a null pointer otherwise.
static shared_ptr<type_composition>
build_type_composition(read_context& ctxt, const xmlNodePtr node,
		       unsigned index, bool update_depth_info)
{
  shared_ptr<type_composition> nil, result;

  if (!xmlStrEqual(node->name, BAD_CAST("template-parameter-type-composition")))
    return nil;

  shared_ptr<type_base> composed_type;
  result.reset(new type_composition(index, composed_type));
  ctxt.push_decl_to_current_scope(dynamic_pointer_cast<decl_base>(result),
				  node, update_depth_info,
				  /*add_to_current_scope=*/true);

  for (xmlNodePtr n = node->children; n; n = n->next)
    {
      if (n->type != XML_ELEMENT_NODE)
	continue;

      if ((composed_type =
	   build_pointer_type_def(ctxt, n,
				  /*update_depth_info=*/true,
				  /*add_to_current_scope=*/true))
	  ||(composed_type =
	     build_reference_type_def(ctxt, n,
				      /*update_depth_info=*/true,
				      /*add_to_current_scope=*/true))
	  || (composed_type =
	      build_qualified_type_decl(ctxt, n,
					/**update_depth_info=*/true,
					/*add_to_current_scope=*/true)))
	{
	  result->set_composed_type(composed_type);
	  break;
	}
    }

  return result;
}

/// Build an instance of non_type_tparameter from a
/// 'template-non-type-parameter' xml element node.
///
/// @param ctxt the context of the parsing.
///
/// @param node the xml node to parse from.
///
/// @param index the index of the parameter.
///
/// @return a pointer to a newly created instance of
/// non_type_tparameter upon successful completion, a null
/// pointer code otherwise.
static shared_ptr<non_type_tparameter>
build_non_type_tparameter(read_context&	ctxt,
				  const xmlNodePtr	node,
				  unsigned		index,
				  bool update_depth_info)
{
  shared_ptr<non_type_tparameter> r;

  if (!xmlStrEqual(node->name, BAD_CAST("template-non-type-parameter")))
    return r;

  string type_id;
  if (xml_char_sptr s = XML_NODE_GET_ATTRIBUTE(node, "type-id"))
    type_id = CHAR_STR(s);
  shared_ptr<type_base> type;
  if (type_id.empty()
      || !(type = ctxt.get_type_decl(type_id)))
    abort();

  string name;
  if (xml_char_sptr s = XML_NODE_GET_ATTRIBUTE(node, "name"))
    name = xml::unescape_xml_string(CHAR_STR(s));

  location loc;
  read_location(ctxt, node,loc);

  r.reset(new non_type_tparameter(index, name, type, loc));
  ctxt.push_decl_to_current_scope(dynamic_pointer_cast<decl_base>(r),
				  node, update_depth_info,
				  /*add_to_current_scope=*/true);

  return r;
}

/// Build an intance of template_tparameter from a
/// 'template-template-parameter' xml element node.
///
/// @param ctxt the context of the parsing.
///
/// @param node the xml node to parse from.
///
/// @param index the index of the template parameter.
///
/// @return a pointer to a new instance of template_tparameter
/// upon successful completion, a null pointer otherwise.
static shared_ptr<template_tparameter>
build_template_tparameter(read_context& ctxt,
			  const xmlNodePtr node,
			  unsigned index,
			  bool update_depth_info)
{
  shared_ptr<template_tparameter> nil;

  if (!xmlStrEqual(node->name, BAD_CAST("template-template-parameter")))
    return nil;

  string id;
  if (xml_char_sptr s = XML_NODE_GET_ATTRIBUTE(node, "id"))
    id = CHAR_STR(s);
  // Bail out if a type with the same ID already exists.
  assert(!id.empty() && !ctxt.get_type_decl(id));

  string type_id;
  if (xml_char_sptr s = XML_NODE_GET_ATTRIBUTE(node, "type-id"))
    type_id = CHAR_STR(s);
  // Bail out if no type with this ID exists.
  if (!type_id.empty()
      && !(dynamic_pointer_cast<template_tparameter>
	   (ctxt.get_type_decl(type_id))))
    abort();

  string name;
  if (xml_char_sptr s = XML_NODE_GET_ATTRIBUTE(node, "name"))
    name = xml::unescape_xml_string(CHAR_STR(s));

  location loc;
  read_location(ctxt, node, loc);

  shared_ptr<template_tparameter> result
    (new template_tparameter(index, name, loc));

  ctxt.push_decl_to_current_scope(result, node, update_depth_info,
				  /*add_to_current_scope=*/true);

  // Go parse template parameters that are children nodes
  int parm_index = 0;
  for (xmlNodePtr n = node->children; n; n = n->next)
    {
      if (node->type != XML_ELEMENT_NODE)
	continue;

      if (shared_ptr<template_parameter> p =
	  build_template_parameter(ctxt, n, parm_index,
				   /*update_depth_info=*/true))
	{
	  result->add_template_parameter(p);
	  ++parm_index;
	}
    }

  if (result)
    ctxt.key_type_decl(result, id);

  return result;
}

/// Build a template parameter type from several possible xml elment
/// nodes representing a serialized form a template parameter.
///
/// @param ctxt the context of the parsing.
///
/// @param node the xml element node to parse from.
///
/// @param index the index of the template parameter we are parsing.
///
/// @return a pointer to a newly created instance of
/// template_parameter upon successful completion, a null pointer
/// otherwise.
static shared_ptr<template_parameter>
build_template_parameter(read_context&		ctxt,
			 const xmlNodePtr	node,
			 unsigned		index,
			 bool update_depth_info)
{
  shared_ptr<template_parameter> r;
  ((r = build_type_tparameter(ctxt, node, index, update_depth_info))
   || (r = build_non_type_tparameter(ctxt, node, index, update_depth_info))
   || (r = build_template_tparameter(ctxt, node, index, update_depth_info))
   || (r = build_type_composition(ctxt, node, index, update_depth_info)));

  return r;
}

/// Build a type from an xml node.
///
/// @param ctxt the context of the parsing.
///
/// @param node the xml node to build the type_base from.
///
/// @return a pointer to the newly built type_base upon successful
/// completion, a null pointer otherwise.
static shared_ptr<type_base>
build_type(read_context& ctxt, const xmlNodePtr node,
	   bool update_depth_info, bool add_to_current_scope)
{
  shared_ptr<type_base> t;

  ((t = build_type_decl(ctxt, node, update_depth_info,
			add_to_current_scope))
   || (t = build_qualified_type_decl(ctxt, node, update_depth_info,
				     add_to_current_scope))
   || (t = build_pointer_type_def(ctxt, node, update_depth_info,
				  add_to_current_scope))
   || (t = build_reference_type_def(ctxt, node ,update_depth_info,
				    add_to_current_scope))
   || (t = build_enum_type_decl(ctxt, node,update_depth_info,
				add_to_current_scope))
   || (t = build_typedef_decl(ctxt, node, update_depth_info,
			      add_to_current_scope))
   || (t = build_class_decl(ctxt, node, update_depth_info,
			    add_to_current_scope)));

  return t;
}

/// Parses 'type-decl' xml element.
///
/// @param ctxt the parsing context.
///
/// @return true upon successful parsing, false otherwise.
static bool
handle_type_decl(read_context& ctxt)
{
  xml::reader_sptr r = ctxt.get_reader();
  if (!r)
    return false;

  xmlNodePtr node = xmlTextReaderExpand(r.get());
  if (!node)
    return false;

  type_decl_sptr decl = build_type_decl(ctxt, node, /*update_depth_info=*/false,
					/*add_to_current_scope=*/true);

  xmlTextReaderNext(r.get());

  return decl;
}

/// Parses 'namespace-decl' xml element.
///
/// @param ctxt the parsing context.
///
/// @return true upon successful parsing, false otherwise.
static bool
handle_namespace_decl(read_context& ctxt)
{
  xml::reader_sptr r = ctxt.get_reader();
  if (!r)
    return false;

  /// If we are not at global scope, then the current scope must
  /// itself be a namespace.
  if (!is_global_scope(ctxt.get_cur_scope())
      && !dynamic_cast<namespace_decl*>(ctxt.get_cur_scope()))
    return false;

  string name;
  if (xml_char_sptr s = XML_READER_GET_ATTRIBUTE(r, "name"))
    name = xml::unescape_xml_string(CHAR_STR(s));

  location loc;
  read_location(ctxt, loc);

  shared_ptr<decl_base> decl(new namespace_decl(name, loc));
  ctxt.push_decl_to_current_scope(decl, /*add_to_current_scope=*/true);
  return true;
}

/// Parse a qualified-type-def xml element.
///
/// @param ctxt the parsing context.
///
/// @return true upon successful parsing, false otherwise.
static bool
handle_qualified_type_decl(read_context& ctxt)
{
 xml::reader_sptr r = ctxt.get_reader();
  if (!r)
    return false;

  string type_id;
  if (xml_char_sptr s = XML_READER_GET_ATTRIBUTE(r, "type-id"))
    type_id = CHAR_STR(s);

  shared_ptr<type_base> underlying_type = ctxt.get_type_decl(type_id);
  assert(underlying_type);

  string id;
  if (xml_char_sptr s = XML_READER_GET_ATTRIBUTE (r, "id"))
    id = CHAR_STR(s);

  assert(!id.empty() && !ctxt.get_type_decl(id));

  string const_str;
  if (xml_char_sptr s = XML_READER_GET_ATTRIBUTE(r, "const"))
    const_str = CHAR_STR(s);
  bool const_cv = const_str == "yes" ? true : false;

  string volatile_str;
  if (xml_char_sptr s = XML_READER_GET_ATTRIBUTE(r, "volatile"))
    volatile_str = CHAR_STR(s);
  bool volatile_cv = volatile_str == "yes" ? true : false;

  qualified_type_def::CV cv = qualified_type_def::CV_NONE;
  if (const_cv)
    cv =
      static_cast<qualified_type_def::CV>(cv | qualified_type_def::CV_CONST);
  if (volatile_cv)
    cv =
      static_cast<qualified_type_def::CV>(cv | qualified_type_def::CV_VOLATILE);

  location loc;
  read_location(ctxt, loc);

  shared_ptr<type_base> decl(new qualified_type_def(underlying_type,
						    cv, loc));
  return ctxt.push_and_key_type_decl(decl, id, /*add_to_current_scope=*/true);
}

/// Parse a pointer-type-decl element.
///
/// @param ctxt the context of the parsing.
///
/// @return true upon successful completion, false otherwise.
static bool
handle_pointer_type_def(read_context& ctxt)
{
   xml::reader_sptr r = ctxt.get_reader();
  if (!r)
    return false;

  string type_id;
  if (xml_char_sptr s = XML_READER_GET_ATTRIBUTE(r, "type-id"))
    type_id = CHAR_STR(s);

  shared_ptr<type_base> pointed_to_type = ctxt.get_type_decl(type_id);
  assert(pointed_to_type);

  size_t size_in_bits = 0, alignment_in_bits = 0;
  if (xml_char_sptr s = XML_READER_GET_ATTRIBUTE(r, "size-in-bits"))
    size_in_bits = atoi(CHAR_STR(s));
  if (xml_char_sptr s = XML_READER_GET_ATTRIBUTE(r, "alignment-in-bits"))
    alignment_in_bits = atoi(CHAR_STR(s));

  string id;
  if (xml_char_sptr s = XML_READER_GET_ATTRIBUTE(r, "id"))
    id = CHAR_STR(s);
  assert (!id.empty() && !ctxt.get_type_decl(id));

  location loc;
  read_location(ctxt, loc);

  shared_ptr<type_base> t(new pointer_type_def(pointed_to_type,
					       size_in_bits,
					       alignment_in_bits,
					       loc));
  return ctxt.push_and_key_type_decl(t, id, /*add_to_current_scope=*/true);
}

/// Parse a reference-type-def element.
///
/// @param ctxt the context of the parsing.
///
/// reference_type_def is added to.
static bool
handle_reference_type_def(read_context& ctxt)
{
  xml::reader_sptr r = ctxt.get_reader();
  if (!r)
    return false;

  string kind;
  if (xml_char_sptr s = XML_READER_GET_ATTRIBUTE(r, "kind"))
    kind = CHAR_STR(s); // this should be either "lvalue" or "rvalue".
  bool is_lvalue = kind == "lvalue" ? true : false;

  string type_id;
  if (xml_char_sptr s = XML_READER_GET_ATTRIBUTE(r, "type-id"))
    type_id = CHAR_STR(s);

  shared_ptr<type_base> pointed_to_type = ctxt.get_type_decl(type_id);
  assert(pointed_to_type);

  size_t size_in_bits = 0, alignment_in_bits = 0;
  if (xml_char_sptr s = XML_READER_GET_ATTRIBUTE(r, "size-in-bits"))
    size_in_bits = atoi(CHAR_STR(s));
  if (xml_char_sptr s = XML_READER_GET_ATTRIBUTE(r, "alignment-in-bits"))
    alignment_in_bits = atoi(CHAR_STR(s));

  string id;
  if (xml_char_sptr s = XML_READER_GET_ATTRIBUTE(r, "id"))
    id = CHAR_STR(s);
  assert(!id.empty() && !ctxt.get_type_decl(id));

  location loc;
  read_location(ctxt, loc);

  shared_ptr<type_base> t(new reference_type_def(pointed_to_type,
						 is_lvalue,
						 size_in_bits,
						 alignment_in_bits,
						 loc));
  return ctxt.push_and_key_type_decl(t, id, /*add_to_current_scope=*/true);
}

/// Parse an enum-decl element.
///
/// @param ctxt the context of the parsing.
static bool
handle_enum_type_decl(read_context& ctxt)
{
  xml::reader_sptr r = ctxt.get_reader();
  if (!r)
    return false;

  xmlNodePtr node = xmlTextReaderExpand(r.get());
  if (!node)
    return false;

  shared_ptr<enum_type_decl> decl =
    build_enum_type_decl(ctxt, node,
			 /*update_depth_info=*/false,
			 /*add_to_current_scope=*/true);

  xmlTextReaderNext(r.get());

  return decl;
}

/// Parse a typedef-decl element.
///
/// @param ctxt the context of the parsing.
static bool
handle_typedef_decl(read_context& ctxt)
{
    xml::reader_sptr r = ctxt.get_reader();
  if (!r)
    return false;

  string name;
  if (xml_char_sptr s = XML_READER_GET_ATTRIBUTE(r, "name"))
    name = xml::unescape_xml_string(CHAR_STR(s));

  string type_id;
  if (xml_char_sptr s = XML_READER_GET_ATTRIBUTE(r, "type-id"))
    type_id = CHAR_STR(s);
  shared_ptr<type_base> underlying_type(ctxt.get_type_decl(type_id));
  assert(underlying_type);

  string id;
  if (xml_char_sptr s = XML_READER_GET_ATTRIBUTE(r, "id"))
    id = CHAR_STR(s);
  assert(!id.empty() && !ctxt.get_type_decl(id));

  location loc;
  read_location(ctxt, loc);

  shared_ptr<type_base> t(new typedef_decl(name, underlying_type, loc));

  return ctxt.push_and_key_type_decl(t, id, /*add_to_current_scope=*/true);
}

/// Parse a var-decl element.
///
/// @param ctxt the context of the parsing.
static bool
handle_var_decl(read_context& ctxt)
{
  xml::reader_sptr r = ctxt.get_reader();
  if (!r)
    return false;

  xmlNodePtr node = xmlTextReaderExpand(r.get());
  if (!node)
    return false;

  shared_ptr<decl_base> decl = build_var_decl(ctxt, node,
					      /*update_depth_info=*/false,
					      /*add_to_current_scope=*/true);
  xmlTextReaderNext(r.get());

  return decl;
}

/// Parse a function-decl element.
///
/// @param ctxt the context of the parsing
///
/// @return true upon successful completion of the parsing, false
/// otherwise.
static bool
handle_function_decl(read_context& ctxt)
{
  xml::reader_sptr r = ctxt.get_reader();
  if (!r)
    return false;

  xmlNodePtr node = xmlTextReaderExpand(r.get());
  if (!node)
    return false;

  shared_ptr<function_decl> fn =
    build_function_decl(ctxt, node, shared_ptr<class_decl>(),
			/*update_depth_info=*/false,
			/*add_to_current_scope=*/true);

  // now advance the xml reader cursor to the xml node after this
  // expanded 'enum-decl' node.
  xmlTextReaderNext(r.get());

  return true;
}

/// Parse a 'class-decl' xml element.
///
/// @param ctxt the context of the parsing.
///
/// @return true upon successful completion of the parsing, false
/// otherwise.
static bool
handle_class_decl(read_context& ctxt)
{
  xml::reader_sptr r = ctxt.get_reader();
  if (!r)
    return false;

  xmlNodePtr node = xmlTextReaderExpand(r.get());
  if (!node)
    return false;

  shared_ptr<class_decl> decl = build_class_decl(ctxt, node,
						 /*update_depth_info=*/false,
						 /*add_to_current_scope=*/true);

  xmlTextReaderNext(r.get());

  return decl;
}

/// Parse a 'function-template-decl' xml element.
///
/// @param ctxt the parsing context.
///
/// @return true upon successful completion of the parsing, false
/// otherwise.
static bool
handle_function_tdecl(read_context& ctxt)
{
  xml::reader_sptr r = ctxt.get_reader();
  if (!r)
    return false;

  xmlNodePtr node = xmlTextReaderExpand(r.get());
  if (!node)
    return false;

  bool is_ok = build_function_tdecl(ctxt, node,
				    /*update_depth_info=*/false,
				    /*add_to_current_scope=*/true);

  xmlTextReaderNext(r.get());

  return is_ok;
}

/// Parse a 'class-template-decl' xml element.
///
/// @param ctxt the context of the parsing.
///
/// @return true upon successful completion, false otherwise.
static bool
handle_class_tdecl(read_context& ctxt)
{
  xml::reader_sptr r = ctxt.get_reader();
  if (!r)
    return false;

  xmlNodePtr node = xmlTextReaderExpand(r.get());
  if (!node)
    return false;

  bool is_ok = build_class_tdecl(ctxt, node,
				 /*update_depth_info=*/false,
				 /*add_to_current_scope=*/true);
  xmlTextReaderNext(r.get());

  return is_ok;
}

/// De-serialize a translation unit from an ABI Instrumentation xml
/// file coming from an input stream.
///
/// @param in a pointer to the input stream.
///
/// @param tu the translation unit resulting from the parsing.  This
/// is populated iff the function returns true.
///
/// @return true upon successful parsing, false otherwise.
bool
read_translation_unit_from_istream(istream* in,
				   translation_unit& tu)
{
  read_context read_ctxt(xml::new_reader_from_istream(in));
  return read_translation_unit_from_input(read_ctxt, tu);
}

/// De-serialize a translation unit from an ABI Instrumentation xml
/// file coming from an input stream.
///
/// @param in a pointer to the input stream.
///
/// @return a pointer to the resulting translation unit.
translation_unit_sptr
read_translation_unit_from_istream(istream* in)
{
  translation_unit_sptr result(new translation_unit(""));
  if (!read_translation_unit_from_istream(in, *result))
    return translation_unit_sptr();
  return result;
}

/// De-serialize a translation unit from an ABI Instrumentation XML
/// file at a given path.
///
/// @param file_path the path to the ABI Instrumentation XML file.
///
/// @return the deserialized translation or NULL if file_path could
/// not be read.  If file_path contains nothing, a non-null
/// translation_unit is returned, but with empty content.
translation_unit_sptr
read_translation_unit_from_file(const string& file_path)
{
  translation_unit_sptr result(new translation_unit(file_path));

  if (!xml_reader::read_translation_unit_from_file(result->get_path(), *result))
    return translation_unit_sptr();
  return result;
}

/// De-serialize a translation unit from an in-memory buffer
/// containing and ABI Instrumentation XML content.
///
/// @param buffer the buffer containing the ABI Instrumentation XML
/// content to parse.
///
/// @return the deserialized translation.
translation_unit_sptr
read_translation_unit_from_buffer(const std::string& buffer)
{
  translation_unit_sptr result(new translation_unit(""));

  if (!xml_reader::read_translation_unit_from_buffer(buffer, *result))
    return translation_unit_sptr();
  return result;
}

template<typename T>
struct array_deleter
{
  void
  operator()(T* a)
  {
    delete [] a;
  }
};//end array_deleter

/// Deserialize an ABI Instrumentation XML file at a given index in a
/// zip archive, and populate a given @ref translation_unit object
/// with the result of that de-serialization.
///
/// @param the @ref translation_unit to populate with the result of
/// the de-serialization.
///
/// @param ar the zip archive to read from.
///
/// @param file_index the index of the ABI Instrumentation XML file to
/// read from the zip archive.
///
/// @return true upon successful completion, false otherwise.
static bool
read_to_translation_unit(translation_unit& tu,
			 zip_sptr ar,
			 int file_index)
{
  if (!ar)
    return false;

  zip_file_sptr f = open_file_in_archive(ar, file_index);
  if (!f)
    return false;

  string input;
  {
    // Allocate a 64K byte buffer to read the archive.
    int buf_size = 64 * 1024;
    shared_ptr<char> buf(new char[buf_size + 1], array_deleter<char>());
    memset(buf.get(), 0, buf_size + 1);
    input.reserve(buf_size);

    while (zip_fread(f.get(), buf.get(), buf_size))
      {
	input.append(buf.get());
	memset(buf.get(), 0, buf_size + 1);
      }
  }

  if (!read_translation_unit_from_buffer(input, tu))
    return false;

  return true;
}

/// Read an ABI corpus from an archive file which is a ZIP archive of
/// several ABI Instrumentation XML files.
///
/// @param ar an object representing the archive file.
///
/// @param corp the ABI Corpus object to populate with the content of
/// the archive @ref ar.
///
/// @return the number of ABI Instrumentation file read from the
/// archive.
static int
read_corpus_from_archive(zip_sptr ar,
			 corpus& corp)
{
  if (!ar)
    return -1;

  int nb_of_tu_read = 0;
  int nb_entries = zip_get_num_entries(ar.get(), 0);
  if (nb_entries < 0)
    return -1;

  // TODO: ensure abi-info descriptor is present in the archive.  Read
  // it and ensure that version numbers match.
  for (int i = 0; i < nb_entries; ++i)
    {
      shared_ptr<translation_unit>
	tu(new translation_unit(zip_get_name(ar.get(), i, 0)));
      if (read_to_translation_unit(*tu, ar, i))
	{
	  corp.add(tu);
	  ++nb_of_tu_read;
	}
    }
  return nb_of_tu_read;
}

/// Read an ABI corpus from an archive file which is a ZIP archive of
/// several ABI Instrumentation XML files.
///
/// @param corp the corpus to populate with the result of reading the
/// archive.
///
/// @param path the path to the archive file.
///
/// @return the number of ABI Instrument XML file read from the
/// archive, or -1 if the file could not read.
int
read_corpus_from_file(corpus& corp,
		      const string& path)
{
  if (path.empty())
    return -1;

  int error_code = 0;
  zip_sptr archive = open_archive(path, ZIP_CREATE|ZIP_CHECKCONS, &error_code);
  if (error_code)
    return -1;

  assert(archive);
  return read_corpus_from_archive(archive, corp);
}

/// Read an ABI corpus from an archive file which is a ZIP archive of
/// several ABI Instrumentation XML files.
///
/// @param corp the corpus to populate with the result of reading the
/// archive.  The archive file to consider is corp.get_path().
///
/// @return the number of ABI Instrument XML file read from the
/// archive.
int
read_corpus_from_file(corpus& corp)
{return read_corpus_from_file(corp, corp.get_path());}

/// Read an ABI corpus from an archive file which is a ZIP archive of
/// several ABI Instrumentation XML files.
///
/// @param path the path to the archive file.
///
/// @return the resulting corpus object, or NULL if the file could not
/// be read.
corpus_sptr
read_corpus_from_file(const string& path)
{
  if (path.empty())
    return corpus_sptr();

  corpus_sptr corp(new corpus(path));
  if (read_corpus_from_file(*corp, path) < 0)
    return corpus_sptr();

  return corp;
}

/// De-serialize an ABI corpus from an input XML document which root
/// node is 'abi-corpus'.
///
/// @param in the input stream to read the XML document from.
///
/// @param corp the corpus de-serialized from the parsing.  This is
/// set iff the function returns true.
///
/// @return true upon successful parsing, false otherwise.
bool
read_corpus_from_native_xml(std::istream* in,
			    corpus& corp)
{
  read_context read_ctxt(xml::new_reader_from_istream(in));
  return read_corpus_from_input(read_ctxt, corp);
}

/// De-serialize an ABI corpus from an input XML document which root
/// node is 'abi-corpus'.
///
/// @param in the input stream to read the XML document from.
///
/// @return the resulting corpus de-serialized from the parsing.  This
/// is non-null iff the parsing resulted in a valid corpus.
corpus_sptr
read_corpus_from_native_xml(std::istream* in)
{
  corpus_sptr corp(new corpus(""));
  if (read_corpus_from_native_xml(in, *corp))
    return corp;

  return corpus_sptr();
}

/// De-serialize an ABI corpus from an XML document file which root
/// node is 'abi-corpus'.
///
/// @param path the path to the input file to read the XML document
/// from.
///
/// @param corp the corpus de-serialized from the parsing.  This is
/// set iff the function returns true.
///
/// @return true upon successful parsing, false otherwise.
bool
read_corpus_from_native_xml_file(corpus& corp,
				 const string& path)
{
  read_context read_ctxt(xml::new_reader_from_file(path));
  return read_corpus_from_input(read_ctxt, corp);
}

/// De-serialize an ABI corpus from an XML document file which root
/// node is 'abi-corpus'.
///
/// @param path the path to the input file to read the XML document
/// from.
///
/// @return the resulting corpus de-serialized from the parsing.  This
/// is non-null if the parsing successfully resulted in a corpus.
corpus_sptr
read_corpus_from_native_xml_file(const string& path)
{
  corpus_sptr corp(new corpus(""));
  if (read_corpus_from_native_xml_file(*corp, path))
    {
      if (corp->get_path().empty())
	corp->set_path(path);
      return corp;
    }
  return corpus_sptr();
}

}//end namespace xml_reader

}//end namespace abigail

/*
   Copyright (c) 2000, 2011, Oracle and/or its affiliates. All rights reserved.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA
*/


/* A lexical scanner on a temporary buffer with a yacc interface */

#define MYSQL_LEX 1
#include "mysql_priv.h"
#include "item_create.h"
#include <m_ctype.h>
#include <hash.h>
#include "sp.h"
#include "sp_head.h"

/*
  We are using pointer to this variable for distinguishing between assignment
  to NEW row field (when parsing trigger definition) and structured variable.
*/

sys_var *trg_new_row_fake_var= (sys_var*) 0x01;

/* Longest standard keyword name */
#define TOCK_NAME_LENGTH 24

/*
  The following data is based on the latin1 character set, and is only
  used when comparing keywords
*/

static uchar to_upper_lex[]=
{
    0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14, 15,
   16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31,
   32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47,
   48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 62, 63,
   64, 65, 66, 67, 68, 69, 70, 71, 72, 73, 74, 75, 76, 77, 78, 79,
   80, 81, 82, 83, 84, 85, 86, 87, 88, 89, 90, 91, 92, 93, 94, 95,
   96, 65, 66, 67, 68, 69, 70, 71, 72, 73, 74, 75, 76, 77, 78, 79,
   80, 81, 82, 83, 84, 85, 86, 87, 88, 89, 90,123,124,125,126,127,
  128,129,130,131,132,133,134,135,136,137,138,139,140,141,142,143,
  144,145,146,147,148,149,150,151,152,153,154,155,156,157,158,159,
  160,161,162,163,164,165,166,167,168,169,170,171,172,173,174,175,
  176,177,178,179,180,181,182,183,184,185,186,187,188,189,190,191,
  192,193,194,195,196,197,198,199,200,201,202,203,204,205,206,207,
  208,209,210,211,212,213,214,215,216,217,218,219,220,221,222,223,
  192,193,194,195,196,197,198,199,200,201,202,203,204,205,206,207,
  208,209,210,211,212,213,214,247,216,217,218,219,220,221,222,255
};

/* 
  Names of the index hints (for error messages). Keep in sync with 
  index_hint_type 
*/

const char * index_hint_type_name[] =
{
  "IGNORE INDEX", 
  "USE INDEX", 
  "FORCE INDEX"
};

inline int lex_casecmp(const char *s, const char *t, uint len)
{
  while (len-- != 0 &&
	 to_upper_lex[(uchar) *s++] == to_upper_lex[(uchar) *t++]) ;
  return (int) len+1;
}

#include <lex_hash.h>


void lex_init(void)
{
  uint i;
  DBUG_ENTER("lex_init");
  for (i=0 ; i < array_elements(symbols) ; i++)
    symbols[i].length=(uchar) strlen(symbols[i].name);
  for (i=0 ; i < array_elements(sql_functions) ; i++)
    sql_functions[i].length=(uchar) strlen(sql_functions[i].name);

  DBUG_VOID_RETURN;
}


void lex_free(void)
{					// Call this when daemon ends
  DBUG_ENTER("lex_free");
  DBUG_VOID_RETURN;
}


void
st_parsing_options::reset()
{
  allows_variable= TRUE;
  allows_select_into= TRUE;
  allows_select_procedure= TRUE;
  allows_derived= TRUE;
}


bool Lex_input_stream::init(THD *thd, char *buff, unsigned int length)
{
  DBUG_EXECUTE_IF("bug42064_simulate_oom",
                  DBUG_SET("+d,simulate_out_of_memory"););

  m_cpp_buf= (char*) thd->alloc(length + 1);

  DBUG_EXECUTE_IF("bug42064_simulate_oom",
                  DBUG_SET("-d,bug42064_simulate_oom");); 

  if (m_cpp_buf == NULL)
    return TRUE;

  m_cpp_ptr= m_cpp_buf;
  m_thd= thd;
  m_ptr= buff;
  m_end_of_query= buff + length;
  m_buf= buff;
  m_buf_length= length;
  ignore_space= test(thd->variables.sql_mode & MODE_IGNORE_SPACE);

  return FALSE;
}


/**
  The operation is called from the parser in order to
  1) designate the intention to have utf8 body;
  1) Indicate to the lexer that we will need a utf8 representation of this
     statement;
  2) Determine the beginning of the body.

  @param thd        Thread context.
  @param begin_ptr  Pointer to the start of the body in the pre-processed
                    buffer.
*/

void Lex_input_stream::body_utf8_start(THD *thd, const char *begin_ptr)
{
  DBUG_ASSERT(begin_ptr);
  DBUG_ASSERT(m_cpp_buf <= begin_ptr && begin_ptr <= m_cpp_buf + m_buf_length);

  uint body_utf8_length=
    (m_buf_length / thd->variables.character_set_client->mbminlen) *
    my_charset_utf8_bin.mbmaxlen;

  m_body_utf8= (char *) thd->alloc(body_utf8_length + 1);
  m_body_utf8_ptr= m_body_utf8;
  *m_body_utf8_ptr= 0;

  m_cpp_utf8_processed_ptr= begin_ptr;
}

/**
  @brief The operation appends unprocessed part of pre-processed buffer till
  the given pointer (ptr) and sets m_cpp_utf8_processed_ptr to end_ptr.

  The idea is that some tokens in the pre-processed buffer (like character
  set introducers) should be skipped.

  Example:
    CPP buffer: SELECT 'str1', _latin1 'str2';
    m_cpp_utf8_processed_ptr -- points at the "SELECT ...";
    In order to skip "_latin1", the following call should be made:
      body_utf8_append(<pointer to "_latin1 ...">, <pointer to " 'str2'...">)

  @param ptr      Pointer in the pre-processed buffer, which specifies the
                  end of the chunk, which should be appended to the utf8
                  body.
  @param end_ptr  Pointer in the pre-processed buffer, to which
                  m_cpp_utf8_processed_ptr will be set in the end of the
                  operation.
*/

void Lex_input_stream::body_utf8_append(const char *ptr,
                                        const char *end_ptr)
{
  DBUG_ASSERT(m_cpp_buf <= ptr && ptr <= m_cpp_buf + m_buf_length);
  DBUG_ASSERT(m_cpp_buf <= end_ptr && end_ptr <= m_cpp_buf + m_buf_length);

  if (!m_body_utf8)
    return;

  if (m_cpp_utf8_processed_ptr >= ptr)
    return;

  int bytes_to_copy= ptr - m_cpp_utf8_processed_ptr;

  memcpy(m_body_utf8_ptr, m_cpp_utf8_processed_ptr, bytes_to_copy);
  m_body_utf8_ptr += bytes_to_copy;
  *m_body_utf8_ptr= 0;

  m_cpp_utf8_processed_ptr= end_ptr;
}

/**
  The operation appends unprocessed part of the pre-processed buffer till
  the given pointer (ptr) and sets m_cpp_utf8_processed_ptr to ptr.

  @param ptr  Pointer in the pre-processed buffer, which specifies the end
              of the chunk, which should be appended to the utf8 body.
*/

void Lex_input_stream::body_utf8_append(const char *ptr)
{
  body_utf8_append(ptr, ptr);
}

/**
  The operation converts the specified text literal to the utf8 and appends
  the result to the utf8-body.

  @param thd      Thread context.
  @param txt      Text literal.
  @param txt_cs   Character set of the text literal.
  @param end_ptr  Pointer in the pre-processed buffer, to which
                  m_cpp_utf8_processed_ptr will be set in the end of the
                  operation.
*/

void Lex_input_stream::body_utf8_append_literal(THD *thd,
                                                const LEX_STRING *txt,
                                                CHARSET_INFO *txt_cs,
                                                const char *end_ptr)
{
  if (!m_cpp_utf8_processed_ptr)
    return;

  LEX_STRING utf_txt;

  if (!my_charset_same(txt_cs, &my_charset_utf8_general_ci))
  {
    thd->convert_string(&utf_txt,
                        &my_charset_utf8_general_ci,
                        txt->str, (uint) txt->length,
                        txt_cs);
  }
  else
  {
    utf_txt.str= txt->str;
    utf_txt.length= txt->length;
  }

  /* NOTE: utf_txt.length is in bytes, not in symbols. */

  memcpy(m_body_utf8_ptr, utf_txt.str, utf_txt.length);
  m_body_utf8_ptr += utf_txt.length;
  *m_body_utf8_ptr= 0;

  m_cpp_utf8_processed_ptr= end_ptr;
}


/*
  This is called before every query that is to be parsed.
  Because of this, it's critical to not do too much things here.
  (We already do too much here)
*/

void lex_start(THD *thd)
{
  LEX *lex= thd->lex;
  DBUG_ENTER("lex_start");

  lex->thd= lex->unit.thd= thd;

  lex->context_stack.empty();
  lex->unit.init_query();
  lex->unit.init_select();
  /* 'parent_lex' is used in init_query() so it must be before it. */
  lex->select_lex.parent_lex= lex;
  lex->select_lex.init_query();
  lex->value_list.empty();
  lex->update_list.empty();
  lex->set_var_list.empty();
  lex->param_list.empty();
  lex->view_list.empty();
  lex->prepared_stmt_params.empty();
  lex->auxiliary_table_list.empty();
  lex->unit.next= lex->unit.master=
    lex->unit.link_next= lex->unit.return_to= 0;
  lex->unit.prev= lex->unit.link_prev= 0;
  lex->unit.slave= lex->unit.global_parameters= lex->current_select=
    lex->all_selects_list= &lex->select_lex;
  lex->select_lex.master= &lex->unit;
  lex->select_lex.prev= &lex->unit.slave;
  lex->select_lex.link_next= lex->select_lex.slave= lex->select_lex.next= 0;
  lex->select_lex.link_prev= (st_select_lex_node**)&(lex->all_selects_list);
  lex->select_lex.options= 0;
  lex->select_lex.sql_cache= SELECT_LEX::SQL_CACHE_UNSPECIFIED;
  lex->select_lex.init_order();
  lex->select_lex.group_list.empty();
  if (lex->select_lex.group_list_ptrs)
    lex->select_lex.group_list_ptrs->clear();
  lex->describe= 0;
  lex->subqueries= FALSE;
  lex->context_analysis_only= 0;
  lex->derived_tables= 0;
  lex->lock_option= TL_READ;
  lex->safe_to_cache_query= 1;
  lex->leaf_tables_insert= 0;
  lex->parsing_options.reset();
  lex->empty_field_list_on_rset= 0;
  lex->select_lex.select_number= 1;
  lex->length=0;
  lex->part_info= 0;
  lex->select_lex.in_sum_expr=0;
  lex->select_lex.ftfunc_list_alloc.empty();
  lex->select_lex.ftfunc_list= &lex->select_lex.ftfunc_list_alloc;
  lex->select_lex.group_list.empty();
  lex->select_lex.order_list.empty();
  lex->sql_command= SQLCOM_END;
  lex->duplicates= DUP_ERROR;
  lex->ignore= 0;
  lex->spname= NULL;
  lex->sphead= NULL;
  lex->spcont= NULL;
  lex->proc_list.first= 0;
  lex->escape_used= FALSE;
  lex->query_tables= 0;
  lex->reset_query_tables_list(FALSE);
  lex->expr_allows_subselect= TRUE;
  lex->use_only_table_context= FALSE;

  lex->name.str= 0;
  lex->name.length= 0;
  lex->event_parse_data= NULL;
  lex->profile_options= PROFILE_NONE;
  lex->nest_level=0 ;
  lex->allow_sum_func= 0;
  lex->in_sum_func= NULL;
  lex->protect_against_global_read_lock= FALSE;
  /*
    ok, there must be a better solution for this, long-term
    I tried "bzero" in the sql_yacc.yy code, but that for
    some reason made the values zero, even if they were set
  */
  lex->server_options.server_name= 0;
  lex->server_options.server_name_length= 0;
  lex->server_options.host= 0;
  lex->server_options.db= 0;
  lex->server_options.username= 0;
  lex->server_options.password= 0;
  lex->server_options.scheme= 0;
  lex->server_options.socket= 0;
  lex->server_options.owner= 0;
  lex->server_options.port= -1;

  lex->is_lex_started= TRUE;
  lex->used_tables= 0;
  DBUG_VOID_RETURN;
}

void lex_end(LEX *lex)
{
  DBUG_ENTER("lex_end");
  DBUG_PRINT("enter", ("lex: 0x%lx", (long) lex));

  /* release used plugins */
  plugin_unlock_list(0, (plugin_ref*)lex->plugins.buffer, 
                     lex->plugins.elements);
  reset_dynamic(&lex->plugins);

  DBUG_VOID_RETURN;
}

Yacc_state::~Yacc_state()
{
  if (yacc_yyss)
  {
    my_free(yacc_yyss, MYF(0));
    my_free(yacc_yyvs, MYF(0));
  }
}

static int find_keyword(Lex_input_stream *lip, uint len, bool function)
{
  const char *tok= lip->get_tok_start();

  SYMBOL *symbol= get_hash_symbol(tok, len, function);
  if (symbol)
  {
    lip->yylval->symbol.symbol=symbol;
    lip->yylval->symbol.str= (char*) tok;
    lip->yylval->symbol.length=len;

    if ((symbol->tok == NOT_SYM) &&
        (lip->m_thd->variables.sql_mode & MODE_HIGH_NOT_PRECEDENCE))
      return NOT2_SYM;
    if ((symbol->tok == OR_OR_SYM) &&
	!(lip->m_thd->variables.sql_mode & MODE_PIPES_AS_CONCAT))
      return OR2_SYM;

    return symbol->tok;
  }
  return 0;
}

/*
  Check if name is a keyword

  SYNOPSIS
    is_keyword()
    name      checked name (must not be empty)
    len       length of checked name

  RETURN VALUES
    0         name is a keyword
    1         name isn't a keyword
*/

bool is_keyword(const char *name, uint len)
{
  DBUG_ASSERT(len != 0);
  return get_hash_symbol(name,len,0)!=0;
}

/**
  Check if name is a sql function

    @param name      checked name

    @return is this a native function or not
    @retval 0         name is a function
    @retval 1         name isn't a function
*/

bool is_lex_native_function(const LEX_STRING *name)
{
  DBUG_ASSERT(name != NULL);
  return (get_hash_symbol(name->str, (uint) name->length, 1) != 0);
}

/* make a copy of token before ptr and set yytoklen */

static LEX_STRING get_token(Lex_input_stream *lip, uint skip, uint length)
{
  LEX_STRING tmp;
  lip->yyUnget();                       // ptr points now after last token char
  tmp.length=lip->yytoklen=length;
  tmp.str= lip->m_thd->strmake(lip->get_tok_start() + skip, tmp.length);

  lip->m_cpp_text_start= lip->get_cpp_tok_start() + skip;
  lip->m_cpp_text_end= lip->m_cpp_text_start + tmp.length;

  return tmp;
}

/* 
 todo: 
   There are no dangerous charsets in mysql for function 
   get_quoted_token yet. But it should be fixed in the 
   future to operate multichar strings (like ucs2)
*/

static LEX_STRING get_quoted_token(Lex_input_stream *lip,
                                   uint skip,
                                   uint length, char quote)
{
  LEX_STRING tmp;
  const char *from, *end;
  char *to;
  lip->yyUnget();                       // ptr points now after last token char
  tmp.length= lip->yytoklen=length;
  tmp.str=(char*) lip->m_thd->alloc(tmp.length+1);
  from= lip->get_tok_start() + skip;
  to= tmp.str;
  end= to+length;

  lip->m_cpp_text_start= lip->get_cpp_tok_start() + skip;
  lip->m_cpp_text_end= lip->m_cpp_text_start + length;

  for ( ; to != end; )
  {
    if ((*to++= *from++) == quote)
    {
      from++;					// Skip double quotes
      lip->m_cpp_text_start++;
    }
  }
  *to= 0;					// End null for safety
  return tmp;
}


/*
  Return an unescaped text literal without quotes
  Fix sometimes to do only one scan of the string
*/

static char *get_text(Lex_input_stream *lip, int pre_skip, int post_skip)
{
  reg1 uchar c,sep;
  uint found_escape=0;
  CHARSET_INFO *cs= lip->m_thd->charset();

  lip->tok_bitmap= 0;
  sep= lip->yyGetLast();                        // String should end with this
  while (! lip->eof())
  {
    c= lip->yyGet();
    lip->tok_bitmap|= c;
#ifdef USE_MB
    {
      int l;
      if (use_mb(cs) &&
          (l = my_ismbchar(cs,
                           lip->get_ptr() -1,
                           lip->get_end_of_query()))) {
        lip->skip_binary(l-1);
        continue;
      }
    }
#endif
    if (c == '\\' &&
        !(lip->m_thd->variables.sql_mode & MODE_NO_BACKSLASH_ESCAPES))
    {					// Escaped character
      found_escape=1;
      if (lip->eof())
	return 0;
      lip->yySkip();
    }
    else if (c == sep)
    {
      if (c == lip->yyGet())            // Check if two separators in a row
      {
        found_escape=1;                 // duplicate. Remember for delete
	continue;
      }
      else
        lip->yyUnget();

      /* Found end. Unescape and return string */
      const char *str, *end;
      char *start;

      str= lip->get_tok_start();
      end= lip->get_ptr();
      /* Extract the text from the token */
      str += pre_skip;
      end -= post_skip;
      DBUG_ASSERT(end >= str);

      if (!(start= (char*) lip->m_thd->alloc((uint) (end-str)+1)))
	return (char*) "";		// Sql_alloc has set error flag

      lip->m_cpp_text_start= lip->get_cpp_tok_start() + pre_skip;
      lip->m_cpp_text_end= lip->get_cpp_ptr() - post_skip;

      if (!found_escape)
      {
	lip->yytoklen=(uint) (end-str);
	memcpy(start,str,lip->yytoklen);
	start[lip->yytoklen]=0;
      }
      else
      {
        char *to;

	for (to=start ; str != end ; str++)
	{
#ifdef USE_MB
	  int l;
	  if (use_mb(cs) &&
              (l = my_ismbchar(cs, str, end))) {
	      while (l--)
		  *to++ = *str++;
	      str--;
	      continue;
	  }
#endif
	  if (!(lip->m_thd->variables.sql_mode & MODE_NO_BACKSLASH_ESCAPES) &&
              *str == '\\' && str+1 != end)
	  {
	    switch(*++str) {
	    case 'n':
	      *to++='\n';
	      break;
	    case 't':
	      *to++= '\t';
	      break;
	    case 'r':
	      *to++ = '\r';
	      break;
	    case 'b':
	      *to++ = '\b';
	      break;
	    case '0':
	      *to++= 0;			// Ascii null
	      break;
	    case 'Z':			// ^Z must be escaped on Win32
	      *to++='\032';
	      break;
	    case '_':
	    case '%':
	      *to++= '\\';		// remember prefix for wildcard
	      /* Fall through */
	    default:
              *to++= *str;
	      break;
	    }
	  }
	  else if (*str == sep)
	    *to++= *str++;		// Two ' or "
	  else
	    *to++ = *str;
	}
	*to=0;
	lip->yytoklen=(uint) (to-start);
      }
      return start;
    }
  }
  return 0;					// unexpected end of query
}


/*
** Calc type of integer; long integer, longlong integer or real.
** Returns smallest type that match the string.
** When using unsigned long long values the result is converted to a real
** because else they will be unexpected sign changes because all calculation
** is done with longlong or double.
*/

static const char *long_str="2147483647";
static const uint long_len=10;
static const char *signed_long_str="-2147483648";
static const char *longlong_str="9223372036854775807";
static const uint longlong_len=19;
static const char *signed_longlong_str="-9223372036854775808";
static const uint signed_longlong_len=19;
static const char *unsigned_longlong_str="18446744073709551615";
static const uint unsigned_longlong_len=20;

static inline uint int_token(const char *str,uint length)
{
  if (length < long_len)			// quick normal case
    return NUM;
  bool neg=0;

  if (*str == '+')				// Remove sign and pre-zeros
  {
    str++; length--;
  }
  else if (*str == '-')
  {
    str++; length--;
    neg=1;
  }
  while (*str == '0' && length)
  {
    str++; length --;
  }
  if (length < long_len)
    return NUM;

  uint smaller,bigger;
  const char *cmp;
  if (neg)
  {
    if (length == long_len)
    {
      cmp= signed_long_str+1;
      smaller=NUM;				// If <= signed_long_str
      bigger=LONG_NUM;				// If >= signed_long_str
    }
    else if (length < signed_longlong_len)
      return LONG_NUM;
    else if (length > signed_longlong_len)
      return DECIMAL_NUM;
    else
    {
      cmp=signed_longlong_str+1;
      smaller=LONG_NUM;				// If <= signed_longlong_str
      bigger=DECIMAL_NUM;
    }
  }
  else
  {
    if (length == long_len)
    {
      cmp= long_str;
      smaller=NUM;
      bigger=LONG_NUM;
    }
    else if (length < longlong_len)
      return LONG_NUM;
    else if (length > longlong_len)
    {
      if (length > unsigned_longlong_len)
        return DECIMAL_NUM;
      cmp=unsigned_longlong_str;
      smaller=ULONGLONG_NUM;
      bigger=DECIMAL_NUM;
    }
    else
    {
      cmp=longlong_str;
      smaller=LONG_NUM;
      bigger= ULONGLONG_NUM;
    }
  }
  while (*cmp && *cmp++ == *str++) ;
  return ((uchar) str[-1] <= (uchar) cmp[-1]) ? smaller : bigger;
}


/**
  Given a stream that is advanced to the first contained character in 
  an open comment, consume the comment.  Optionally, if we are allowed, 
  recurse so that we understand comments within this current comment.

  At this level, we do not support version-condition comments.  We might 
  have been called with having just passed one in the stream, though.  In 
  that case, we probably want to tolerate mundane comments inside.  Thus,
  the case for recursion.

  @retval  Whether EOF reached before comment is closed.
*/
bool consume_comment(Lex_input_stream *lip, int remaining_recursions_permitted)
{
  reg1 uchar c;
  while (! lip->eof())
  {
    c= lip->yyGet();

    if (remaining_recursions_permitted > 0)
    {
      if ((c == '/') && (lip->yyPeek() == '*'))
      {
        lip->yySkip(); /* Eat asterisk */
        consume_comment(lip, remaining_recursions_permitted-1);
        continue;
      }
    }

    if (c == '*')
    {
      if (lip->yyPeek() == '/')
      {
        lip->yySkip(); /* Eat slash */
        return FALSE;
      }
    }

    if (c == '\n')
      lip->yylineno++;
  }

  return TRUE;
}


/*
  MYSQLlex remember the following states from the following MYSQLlex()

  - MY_LEX_EOQ			Found end of query
  - MY_LEX_OPERATOR_OR_IDENT	Last state was an ident, text or number
				(which can't be followed by a signed number)
*/

int MYSQLlex(void *arg, void *yythd)
{
  reg1	uchar c= 0;
  bool comment_closed;
  int	tokval, result_state;
  uint length;
  enum my_lex_states state;
  THD *thd= (THD *)yythd;
  Lex_input_stream *lip= & thd->m_parser_state->m_lip;
  LEX *lex= thd->lex;
  YYSTYPE *yylval=(YYSTYPE*) arg;
  CHARSET_INFO *cs= thd->charset();
  uchar *state_map= cs->state_map;
  uchar *ident_map= cs->ident_map;

  lip->yylval=yylval;			// The global state

  lip->start_token();
  state=lip->next_state;
  lip->next_state=MY_LEX_OPERATOR_OR_IDENT;
  for (;;)
  {
    switch (state) {
    case MY_LEX_OPERATOR_OR_IDENT:	// Next is operator or keyword
    case MY_LEX_START:			// Start of token
      // Skip starting whitespace
      while(state_map[c= lip->yyPeek()] == MY_LEX_SKIP)
      {
	if (c == '\n')
	  lip->yylineno++;

        lip->yySkip();
      }

      /* Start of real token */
      lip->restart_token();
      c= lip->yyGet();
      state= (enum my_lex_states) state_map[c];
      break;
    case MY_LEX_ESCAPE:
      if (lip->yyGet() == 'N')
      {					// Allow \N as shortcut for NULL
	yylval->lex_str.str=(char*) "\\N";
	yylval->lex_str.length=2;
	return NULL_SYM;
      }
    case MY_LEX_CHAR:			// Unknown or single char token
    case MY_LEX_SKIP:			// This should not happen
      if (c == '-' && lip->yyPeek() == '-' &&
          (my_isspace(cs,lip->yyPeekn(1)) ||
           my_iscntrl(cs,lip->yyPeekn(1))))
      {
        state=MY_LEX_COMMENT;
        break;
      }

      if (c != ')')
	lip->next_state= MY_LEX_START;	// Allow signed numbers

      if (c == ',')
      {
        /*
          Warning:
          This is a work around, to make the "remember_name" rule in
          sql/sql_yacc.yy work properly.
          The problem is that, when parsing "select expr1, expr2",
          the code generated by bison executes the *pre* action
          remember_name (see select_item) *before* actually parsing the
          first token of expr2.
        */
        lip->restart_token();
      }
      else
      {
        /*
          Check for a placeholder: it should not precede a possible identifier
          because of binlogging: when a placeholder is replaced with
          its value in a query for the binlog, the query must stay
          grammatically correct.
        */
        if (c == '?' && lip->stmt_prepare_mode && !ident_map[lip->yyPeek()])
        return(PARAM_MARKER);
      }

      return((int) c);

    case MY_LEX_IDENT_OR_NCHAR:
      if (lip->yyPeek() != '\'')
      {
	state= MY_LEX_IDENT;
	break;
      }
      /* Found N'string' */
      lip->yySkip();                         // Skip '
      if (!(yylval->lex_str.str = get_text(lip, 2, 1)))
      {
	state= MY_LEX_CHAR;             // Read char by char
	break;
      }
      yylval->lex_str.length= lip->yytoklen;
      lex->text_string_is_7bit= (lip->tok_bitmap & 0x80) ? 0 : 1;
      return(NCHAR_STRING);

    case MY_LEX_IDENT_OR_HEX:
      if (lip->yyPeek() == '\'')
      {					// Found x'hex-number'
	state= MY_LEX_HEX_NUMBER;
	break;
      }
    case MY_LEX_IDENT_OR_BIN:
      if (lip->yyPeek() == '\'')
      {                                 // Found b'bin-number'
        state= MY_LEX_BIN_NUMBER;
        break;
      }
    case MY_LEX_IDENT:
      const char *start;
#if defined(USE_MB) && defined(USE_MB_IDENT)
      if (use_mb(cs))
      {
	result_state= IDENT_QUOTED;
        if (my_mbcharlen(cs, lip->yyGetLast()) > 1)
        {
          int l = my_ismbchar(cs,
                              lip->get_ptr() -1,
                              lip->get_end_of_query());
          if (l == 0) {
            state = MY_LEX_CHAR;
            continue;
          }
          lip->skip_binary(l - 1);
        }
        while (ident_map[c=lip->yyGet()])
        {
          if (my_mbcharlen(cs, c) > 1)
          {
            int l;
            if ((l = my_ismbchar(cs,
                                 lip->get_ptr() -1,
                                 lip->get_end_of_query())) == 0)
              break;
            lip->skip_binary(l-1);
          }
        }
      }
      else
#endif
      {
        for (result_state= c; ident_map[c= lip->yyGet()]; result_state|= c) ;
        /* If there were non-ASCII characters, mark that we must convert */
        result_state= result_state & 0x80 ? IDENT_QUOTED : IDENT;
      }
      length= lip->yyLength();
      start= lip->get_ptr();
      if (lip->ignore_space)
      {
        /*
          If we find a space then this can't be an identifier. We notice this
          below by checking start != lex->ptr.
        */
        for (; state_map[c] == MY_LEX_SKIP ; c= lip->yyGet()) ;
      }
      if (start == lip->get_ptr() && c == '.' && ident_map[lip->yyPeek()])
	lip->next_state=MY_LEX_IDENT_SEP;
      else
      {					// '(' must follow directly if function
        lip->yyUnget();
	if ((tokval = find_keyword(lip, length, c == '(')))
	{
	  lip->next_state= MY_LEX_START;	// Allow signed numbers
	  return(tokval);		// Was keyword
	}
        lip->yySkip();                  // next state does a unget
      }
      yylval->lex_str=get_token(lip, 0, length);

      /*
         Note: "SELECT _bla AS 'alias'"
         _bla should be considered as a IDENT if charset haven't been found.
         So we don't use MYF(MY_WME) with get_charset_by_csname to avoid
         producing an error.
      */

      if (yylval->lex_str.str[0] == '_')
      {
        CHARSET_INFO *cs= get_charset_by_csname(yylval->lex_str.str + 1,
                                                MY_CS_PRIMARY, MYF(0));
        if (cs)
        {
          yylval->charset= cs;
          lip->m_underscore_cs= cs;

          lip->body_utf8_append(lip->m_cpp_text_start,
                                lip->get_cpp_tok_start() + length);
          return(UNDERSCORE_CHARSET);
        }
      }

      lip->body_utf8_append(lip->m_cpp_text_start);

      lip->body_utf8_append_literal(thd, &yylval->lex_str, cs,
                                    lip->m_cpp_text_end);

      return(result_state);			// IDENT or IDENT_QUOTED

    case MY_LEX_IDENT_SEP:		// Found ident and now '.'
      yylval->lex_str.str= (char*) lip->get_ptr();
      yylval->lex_str.length= 1;
      c= lip->yyGet();                  // should be '.'
      lip->next_state= MY_LEX_IDENT_START;// Next is an ident (not a keyword)
      if (!ident_map[lip->yyPeek()])            // Probably ` or "
	lip->next_state= MY_LEX_START;
      return((int) c);

    case MY_LEX_NUMBER_IDENT:		// number or ident which num-start
      if (lip->yyGetLast() == '0')
      {
        c= lip->yyGet();
        if (c == 'x')
        {
          while (my_isxdigit(cs,(c = lip->yyGet()))) ;
          if ((lip->yyLength() >= 3) && !ident_map[c])
          {
            /* skip '0x' */
            yylval->lex_str=get_token(lip, 2, lip->yyLength()-2);
            return (HEX_NUM);
          }
          lip->yyUnget();
          state= MY_LEX_IDENT_START;
          break;
        }
        else if (c == 'b')
        {
          while ((c= lip->yyGet()) == '0' || c == '1') ;
          if ((lip->yyLength() >= 3) && !ident_map[c])
          {
            /* Skip '0b' */
            yylval->lex_str= get_token(lip, 2, lip->yyLength()-2);
            return (BIN_NUM);
          }
          lip->yyUnget();
          state= MY_LEX_IDENT_START;
          break;
        }
        lip->yyUnget();
      }

      while (my_isdigit(cs, (c = lip->yyGet()))) ;
      if (!ident_map[c])
      {					// Can't be identifier
	state=MY_LEX_INT_OR_REAL;
	break;
      }
      if (c == 'e' || c == 'E')
      {
	// The following test is written this way to allow numbers of type 1e1
        if (my_isdigit(cs,lip->yyPeek()) ||
            (c=(lip->yyGet())) == '+' || c == '-')
	{				// Allow 1E+10
          if (my_isdigit(cs,lip->yyPeek()))     // Number must have digit after sign
	  {
            lip->yySkip();
            while (my_isdigit(cs,lip->yyGet())) ;
            yylval->lex_str=get_token(lip, 0, lip->yyLength());
	    return(FLOAT_NUM);
	  }
	}
        lip->yyUnget();
      }
      // fall through
    case MY_LEX_IDENT_START:			// We come here after '.'
      result_state= IDENT;
#if defined(USE_MB) && defined(USE_MB_IDENT)
      if (use_mb(cs))
      {
	result_state= IDENT_QUOTED;
        while (ident_map[c=lip->yyGet()])
        {
          if (my_mbcharlen(cs, c) > 1)
          {
            int l;
            if ((l = my_ismbchar(cs,
                                 lip->get_ptr() -1,
                                 lip->get_end_of_query())) == 0)
              break;
            lip->skip_binary(l-1);
          }
        }
      }
      else
#endif
      {
        for (result_state=0; ident_map[c= lip->yyGet()]; result_state|= c) ;
        /* If there were non-ASCII characters, mark that we must convert */
        result_state= result_state & 0x80 ? IDENT_QUOTED : IDENT;
      }
      if (c == '.' && ident_map[lip->yyPeek()])
	lip->next_state=MY_LEX_IDENT_SEP;// Next is '.'

      yylval->lex_str= get_token(lip, 0, lip->yyLength());

      lip->body_utf8_append(lip->m_cpp_text_start);

      lip->body_utf8_append_literal(thd, &yylval->lex_str, cs,
                                    lip->m_cpp_text_end);

      return(result_state);

    case MY_LEX_USER_VARIABLE_DELIMITER:	// Found quote char
    {
      uint double_quotes= 0;
      char quote_char= c;                       // Used char
      while ((c=lip->yyGet()))
      {
	int var_length;
	if ((var_length= my_mbcharlen(cs, c)) == 1)
	{
	  if (c == quote_char)
	  {
            if (lip->yyPeek() != quote_char)
	      break;
            c=lip->yyGet();
	    double_quotes++;
	    continue;
	  }
	}
#ifdef USE_MB
        else if (use_mb(cs))
        {
          if ((var_length= my_ismbchar(cs, lip->get_ptr() - 1,
                                       lip->get_end_of_query())))
            lip->skip_binary(var_length-1);
        }
#endif
      }
      if (double_quotes)
	yylval->lex_str=get_quoted_token(lip, 1,
                                         lip->yyLength() - double_quotes -1,
					 quote_char);
      else
        yylval->lex_str=get_token(lip, 1, lip->yyLength() -1);
      if (c == quote_char)
        lip->yySkip();                  // Skip end `
      lip->next_state= MY_LEX_START;

      lip->body_utf8_append(lip->m_cpp_text_start);

      lip->body_utf8_append_literal(thd, &yylval->lex_str, cs,
                                    lip->m_cpp_text_end);

      return(IDENT_QUOTED);
    }
    case MY_LEX_INT_OR_REAL:		// Complete int or incomplete real
      if (c != '.')
      {					// Found complete integer number.
        yylval->lex_str=get_token(lip, 0, lip->yyLength());
	return int_token(yylval->lex_str.str, (uint) yylval->lex_str.length);
      }
      // fall through
    case MY_LEX_REAL:			// Incomplete real number
      while (my_isdigit(cs,c = lip->yyGet())) ;

      if (c == 'e' || c == 'E')
      {
        c = lip->yyGet();
	if (c == '-' || c == '+')
          c = lip->yyGet();                     // Skip sign
	if (!my_isdigit(cs,c))
	{				// No digit after sign
	  state= MY_LEX_CHAR;
	  break;
	}
        while (my_isdigit(cs,lip->yyGet())) ;
        yylval->lex_str=get_token(lip, 0, lip->yyLength());
	return(FLOAT_NUM);
      }
      yylval->lex_str=get_token(lip, 0, lip->yyLength());
      return(DECIMAL_NUM);

    case MY_LEX_HEX_NUMBER:		// Found x'hexstring'
      lip->yySkip();                    // Accept opening '
      while (my_isxdigit(cs, (c= lip->yyGet()))) ;
      if (c != '\'')
        return(ABORT_SYM);              // Illegal hex constant
      lip->yySkip();                    // Accept closing '
      length= lip->yyLength();          // Length of hexnum+3
      if ((length % 2) == 0)
        return(ABORT_SYM);              // odd number of hex digits
      yylval->lex_str=get_token(lip,
                                2,          // skip x'
                                length-3);  // don't count x' and last '
      return (HEX_NUM);

    case MY_LEX_BIN_NUMBER:           // Found b'bin-string'
      lip->yySkip();                  // Accept opening '
      while ((c= lip->yyGet()) == '0' || c == '1') ;
      if (c != '\'')
        return(ABORT_SYM);            // Illegal hex constant
      lip->yySkip();                  // Accept closing '
      length= lip->yyLength();        // Length of bin-num + 3
      yylval->lex_str= get_token(lip,
                                 2,         // skip b'
                                 length-3); // don't count b' and last '
      return (BIN_NUM);

    case MY_LEX_CMP_OP:			// Incomplete comparison operator
      if (state_map[lip->yyPeek()] == MY_LEX_CMP_OP ||
          state_map[lip->yyPeek()] == MY_LEX_LONG_CMP_OP)
        lip->yySkip();
      if ((tokval = find_keyword(lip, lip->yyLength() + 1, 0)))
      {
	lip->next_state= MY_LEX_START;	// Allow signed numbers
	return(tokval);
      }
      state = MY_LEX_CHAR;		// Something fishy found
      break;

    case MY_LEX_LONG_CMP_OP:		// Incomplete comparison operator
      if (state_map[lip->yyPeek()] == MY_LEX_CMP_OP ||
          state_map[lip->yyPeek()] == MY_LEX_LONG_CMP_OP)
      {
        lip->yySkip();
        if (state_map[lip->yyPeek()] == MY_LEX_CMP_OP)
          lip->yySkip();
      }
      if ((tokval = find_keyword(lip, lip->yyLength() + 1, 0)))
      {
	lip->next_state= MY_LEX_START;	// Found long op
	return(tokval);
      }
      state = MY_LEX_CHAR;		// Something fishy found
      break;

    case MY_LEX_BOOL:
      if (c != lip->yyPeek())
      {
	state=MY_LEX_CHAR;
	break;
      }
      lip->yySkip();
      tokval = find_keyword(lip,2,0);	// Is a bool operator
      lip->next_state= MY_LEX_START;	// Allow signed numbers
      return(tokval);

    case MY_LEX_STRING_OR_DELIMITER:
      if (thd->variables.sql_mode & MODE_ANSI_QUOTES)
      {
	state= MY_LEX_USER_VARIABLE_DELIMITER;
	break;
      }
      /* " used for strings */
    case MY_LEX_STRING:			// Incomplete text string
      if (!(yylval->lex_str.str = get_text(lip, 1, 1)))
      {
	state= MY_LEX_CHAR;		// Read char by char
	break;
      }
      yylval->lex_str.length=lip->yytoklen;

      lip->body_utf8_append(lip->m_cpp_text_start);

      lip->body_utf8_append_literal(thd, &yylval->lex_str,
        lip->m_underscore_cs ? lip->m_underscore_cs : cs,
        lip->m_cpp_text_end);

      lip->m_underscore_cs= NULL;

      lex->text_string_is_7bit= (lip->tok_bitmap & 0x80) ? 0 : 1;
      return(TEXT_STRING);

    case MY_LEX_COMMENT:			//  Comment
      lex->select_lex.options|= OPTION_FOUND_COMMENT;
      while ((c = lip->yyGet()) != '\n' && c) ;
      lip->yyUnget();                   // Safety against eof
      state = MY_LEX_START;		// Try again
      break;
    case MY_LEX_LONG_COMMENT:		/* Long C comment? */
      if (lip->yyPeek() != '*')
      {
	state=MY_LEX_CHAR;		// Probable division
	break;
      }
      lex->select_lex.options|= OPTION_FOUND_COMMENT;
      /* Reject '/' '*', since we might need to turn off the echo */
      lip->yyUnget();

      lip->save_in_comment_state();

      if (lip->yyPeekn(2) == '!')
      {
        lip->in_comment= DISCARD_COMMENT;
        /* Accept '/' '*' '!', but do not keep this marker. */
        lip->set_echo(FALSE);
        lip->yySkip();
        lip->yySkip();
        lip->yySkip();

        /*
          The special comment format is very strict:
          '/' '*' '!', followed by exactly
          1 digit (major), 2 digits (minor), then 2 digits (dot).
          32302 -> 3.23.02
          50032 -> 5.0.32
          50114 -> 5.1.14
        */
        char version_str[6];
        version_str[0]= lip->yyPeekn(0);
        version_str[1]= lip->yyPeekn(1);
        version_str[2]= lip->yyPeekn(2);
        version_str[3]= lip->yyPeekn(3);
        version_str[4]= lip->yyPeekn(4);
        version_str[5]= 0;
        if (  my_isdigit(cs, version_str[0])
           && my_isdigit(cs, version_str[1])
           && my_isdigit(cs, version_str[2])
           && my_isdigit(cs, version_str[3])
           && my_isdigit(cs, version_str[4])
           )
        {
          ulong version;
          version=strtol(version_str, NULL, 10);

          if (version <= MYSQL_VERSION_ID)
          {
            /* Accept 'M' 'm' 'm' 'd' 'd' */
            lip->yySkipn(5);
            /* Expand the content of the special comment as real code */
            lip->set_echo(TRUE);
            state=MY_LEX_START;
            break;  /* Do not treat contents as a comment.  */
          }
          else
          {
            /*
              Patch and skip the conditional comment to avoid it
              being propagated infinitely (eg. to a slave).
            */
            char *pcom= lip->yyUnput(' ');
            comment_closed= ! consume_comment(lip, 1);
            if (! comment_closed)
            {
              *pcom= '!';
            }
            /* version allowed to have one level of comment inside. */
          }
        }
        else
        {
          /* Not a version comment. */
          state=MY_LEX_START;
          lip->set_echo(TRUE);
          break;
        }
      }
      else
      {
        lip->in_comment= PRESERVE_COMMENT;
        lip->yySkip();                  // Accept /
        lip->yySkip();                  // Accept *
        comment_closed= ! consume_comment(lip, 0);
        /* regular comments can have zero comments inside. */
      }
      /*
        Discard:
        - regular '/' '*' comments,
        - special comments '/' '*' '!' for a future version,
        by scanning until we find a closing '*' '/' marker.

        Nesting regular comments isn't allowed.  The first 
        '*' '/' returns the parser to the previous state.

        /#!VERSI oned containing /# regular #/ is allowed #/

		Inside one versioned comment, another versioned comment
		is treated as a regular discardable comment.  It gets
		no special parsing.
      */

      /* Unbalanced comments with a missing '*' '/' are a syntax error */
      if (! comment_closed)
        return (ABORT_SYM);
      state = MY_LEX_START;             // Try again
      lip->restore_in_comment_state();
      break;
    case MY_LEX_END_LONG_COMMENT:
      if ((lip->in_comment != NO_COMMENT) && lip->yyPeek() == '/')
      {
        /* Reject '*' '/' */
        lip->yyUnget();
        /* Accept '*' '/', with the proper echo */
        lip->set_echo(lip->in_comment == PRESERVE_COMMENT);
        lip->yySkipn(2);
        /* And start recording the tokens again */
        lip->set_echo(TRUE);
        lip->in_comment=NO_COMMENT;
        state=MY_LEX_START;
      }
      else
	state=MY_LEX_CHAR;		// Return '*'
      break;
    case MY_LEX_SET_VAR:		// Check if ':='
      if (lip->yyPeek() != '=')
      {
	state=MY_LEX_CHAR;		// Return ':'
	break;
      }
      lip->yySkip();
      return (SET_VAR);
    case MY_LEX_SEMICOLON:			// optional line terminator
      state= MY_LEX_CHAR;               // Return ';'
      break;
    case MY_LEX_EOL:
      if (lip->eof())
      {
        lip->yyUnget();                 // Reject the last '\0'
        lip->set_echo(FALSE);
        lip->yySkip();
        lip->set_echo(TRUE);
        /* Unbalanced comments with a missing '*' '/' are a syntax error */
        if (lip->in_comment != NO_COMMENT)
          return (ABORT_SYM);
        lip->next_state=MY_LEX_END;     // Mark for next loop
        return(END_OF_INPUT);
      }
      state=MY_LEX_CHAR;
      break;
    case MY_LEX_END:
      lip->next_state=MY_LEX_END;
      return(0);			// We found end of input last time

      /* Actually real shouldn't start with . but allow them anyhow */
    case MY_LEX_REAL_OR_POINT:
      if (my_isdigit(cs,lip->yyPeek()))
	state = MY_LEX_REAL;		// Real
      else
      {
	state= MY_LEX_IDENT_SEP;	// return '.'
        lip->yyUnget();                 // Put back '.'
      }
      break;
    case MY_LEX_USER_END:		// end '@' of user@hostname
      switch (state_map[lip->yyPeek()]) {
      case MY_LEX_STRING:
      case MY_LEX_USER_VARIABLE_DELIMITER:
      case MY_LEX_STRING_OR_DELIMITER:
	break;
      case MY_LEX_USER_END:
	lip->next_state=MY_LEX_SYSTEM_VAR;
	break;
      default:
	lip->next_state=MY_LEX_HOSTNAME;
	break;
      }
      yylval->lex_str.str=(char*) lip->get_ptr();
      yylval->lex_str.length=1;
      return((int) '@');
    case MY_LEX_HOSTNAME:		// end '@' of user@hostname
      for (c=lip->yyGet() ;
	   my_isalnum(cs,c) || c == '.' || c == '_' ||  c == '$';
           c= lip->yyGet()) ;
      yylval->lex_str=get_token(lip, 0, lip->yyLength());
      return(LEX_HOSTNAME);
    case MY_LEX_SYSTEM_VAR:
      yylval->lex_str.str=(char*) lip->get_ptr();
      yylval->lex_str.length=1;
      lip->yySkip();                                    // Skip '@'
      lip->next_state= (state_map[lip->yyPeek()] ==
			MY_LEX_USER_VARIABLE_DELIMITER ?
			MY_LEX_OPERATOR_OR_IDENT :
			MY_LEX_IDENT_OR_KEYWORD);
      return((int) '@');
    case MY_LEX_IDENT_OR_KEYWORD:
      /*
	We come here when we have found two '@' in a row.
	We should now be able to handle:
	[(global | local | session) .]variable_name
      */

      for (result_state= 0; ident_map[c= lip->yyGet()]; result_state|= c) ;
      /* If there were non-ASCII characters, mark that we must convert */
      result_state= result_state & 0x80 ? IDENT_QUOTED : IDENT;

      if (c == '.')
	lip->next_state=MY_LEX_IDENT_SEP;
      length= lip->yyLength();
      if (length == 0)
        return(ABORT_SYM);              // Names must be nonempty.
      if ((tokval= find_keyword(lip, length,0)))
      {
        lip->yyUnget();                         // Put back 'c'
	return(tokval);				// Was keyword
      }
      yylval->lex_str=get_token(lip, 0, length);

      lip->body_utf8_append(lip->m_cpp_text_start);

      lip->body_utf8_append_literal(thd, &yylval->lex_str, cs,
                                    lip->m_cpp_text_end);

      return(result_state);
    }
  }
}


/**
  Construct a copy of this object to be used for mysql_alter_table
  and mysql_create_table.

  Historically, these two functions modify their Alter_info
  arguments. This behaviour breaks re-execution of prepared
  statements and stored procedures and is compensated by always
  supplying a copy of Alter_info to these functions.

  @return You need to use check the error in THD for out
  of memory condition after calling this function.
*/

Alter_info::Alter_info(const Alter_info &rhs, MEM_ROOT *mem_root)
  :drop_list(rhs.drop_list, mem_root),
  alter_list(rhs.alter_list, mem_root),
  key_list(rhs.key_list, mem_root),
  create_list(rhs.create_list, mem_root),
  flags(rhs.flags),
  keys_onoff(rhs.keys_onoff),
  tablespace_op(rhs.tablespace_op),
  partition_names(rhs.partition_names, mem_root),
  no_parts(rhs.no_parts),
  change_level(rhs.change_level),
  datetime_field(rhs.datetime_field),
  error_if_not_empty(rhs.error_if_not_empty)
{
  /*
    Make deep copies of used objects.
    This is not a fully deep copy - clone() implementations
    of Alter_drop, Alter_column, Key, foreign_key, Key_part_spec
    do not copy string constants. At the same length the only
    reason we make a copy currently is that ALTER/CREATE TABLE
    code changes input Alter_info definitions, but string
    constants never change.
  */
  list_copy_and_replace_each_value(drop_list, mem_root);
  list_copy_and_replace_each_value(alter_list, mem_root);
  list_copy_and_replace_each_value(key_list, mem_root);
  list_copy_and_replace_each_value(create_list, mem_root);
  /* partition_names are not deeply copied currently */
}


void trim_whitespace(CHARSET_INFO *cs, LEX_STRING *str)
{
  /*
    TODO:
    This code assumes that there are no multi-bytes characters
    that can be considered white-space.
  */

  while ((str->length > 0) && (my_isspace(cs, str->str[0])))
  {
    str->length --;
    str->str ++;
  }

  /*
    FIXME:
    Also, parsing backward is not safe with multi bytes characters
  */
  while ((str->length > 0) && (my_isspace(cs, str->str[str->length-1])))
  {
    str->length --;
  }
}


/*
  st_select_lex structures initialisations
*/

void st_select_lex_node::init_query()
{
  options= 0;
  sql_cache= SQL_CACHE_UNSPECIFIED;
  linkage= UNSPECIFIED_TYPE;
  no_error= no_table_names_allowed= 0;
  uncacheable= 0;
}

void st_select_lex_node::init_select()
{
}

void st_select_lex_unit::init_query()
{
  st_select_lex_node::init_query();
  linkage= GLOBAL_OPTIONS_TYPE;
  global_parameters= first_select();
  select_limit_cnt= HA_POS_ERROR;
  offset_limit_cnt= 0;
  union_distinct= 0;
  prepared= optimized= executed= 0;
  item= 0;
  union_result= 0;
  table= 0;
  fake_select_lex= 0;
  cleaned= 0;
  item_list.empty();
  describe= 0;
  found_rows_for_union= 0;
}

void st_select_lex::init_query()
{
  st_select_lex_node::init_query();
  table_list.empty();
  top_join_list.empty();
  join_list= &top_join_list;
  embedding= leaf_tables= 0;
  item_list.empty();
  join= 0;
  having= prep_having= where= prep_where= 0;
  olap= UNSPECIFIED_OLAP_TYPE;
  having_fix_field= 0;
  group_fix_field= 0;
  context.select_lex= this;
  context.init();
  /*
    Add the name resolution context of the current (sub)query to the
    stack of contexts for the whole query.
    TODO:
    push_context may return an error if there is no memory for a new
    element in the stack, however this method has no return value,
    thus push_context should be moved to a place where query
    initialization is checked for failure.
  */
  parent_lex->push_context(&context);
  cond_count= between_count= with_wild= 0;
  max_equal_elems= 0;
  conds_processed_with_permanent_arena= 0;
  ref_pointer_array= 0;
  select_n_where_fields= 0;
  select_n_having_items= 0;
  n_child_sum_items= 0;
  subquery_in_having= explicit_limit= 0;
  is_item_list_lookup= 0;
  first_execution= 1;
  first_natural_join_processing= 1;
  first_cond_optimization= 1;
  parsing_place= NO_MATTER;
  exclude_from_table_unique_test= no_wrap_view_item= FALSE;
  nest_level= 0;
  link_next= 0;
  lock_option= TL_READ_DEFAULT;
  m_non_agg_field_used= false;
  m_agg_func_used= false;
}

void st_select_lex::init_select()
{
  st_select_lex_node::init_select();
  group_list.empty();
  if (group_list_ptrs)
    group_list_ptrs->clear();
  type= db= 0;
  having= 0;
  table_join_options= 0;
  in_sum_expr= with_wild= 0;
  options= 0;
  sql_cache= SQL_CACHE_UNSPECIFIED;
  braces= 0;
  interval_list.empty();
  ftfunc_list_alloc.empty();
  inner_sum_func_list= 0;
  ftfunc_list= &ftfunc_list_alloc;
  linkage= UNSPECIFIED_TYPE;
  order_list.elements= 0;
  order_list.first= 0;
  order_list.next= &order_list.first;
  /* Set limit and offset to default values */
  select_limit= 0;      /* denotes the default limit = HA_POS_ERROR */
  offset_limit= 0;      /* denotes the default offset = 0 */
  with_sum_func= 0;
  is_correlated= 0;
  cur_pos_in_select_list= UNDEF_POS;
  non_agg_fields.empty();
  cond_value= having_value= Item::COND_UNDEF;
  inner_refs_list.empty();
  m_non_agg_field_used= false;
  m_agg_func_used= false;
}

/*
  st_select_lex structures linking
*/

/* include on level down */
void st_select_lex_node::include_down(st_select_lex_node *upper)
{
  if ((next= upper->slave))
    next->prev= &next;
  prev= &upper->slave;
  upper->slave= this;
  master= upper;
  slave= 0;
}

/*
  include on level down (but do not link)

  SYNOPSYS
    st_select_lex_node::include_standalone()
    upper - reference on node underr which this node should be included
    ref - references on reference on this node
*/
void st_select_lex_node::include_standalone(st_select_lex_node *upper,
					    st_select_lex_node **ref)
{
  next= 0;
  prev= ref;
  master= upper;
  slave= 0;
}

/* include neighbour (on same level) */
void st_select_lex_node::include_neighbour(st_select_lex_node *before)
{
  if ((next= before->next))
    next->prev= &next;
  prev= &before->next;
  before->next= this;
  master= before->master;
  slave= 0;
}

/* including in global SELECT_LEX list */
void st_select_lex_node::include_global(st_select_lex_node **plink)
{
  if ((link_next= *plink))
    link_next->link_prev= &link_next;
  link_prev= plink;
  *plink= this;
}

//excluding from global list (internal function)
void st_select_lex_node::fast_exclude()
{
  if (link_prev)
  {
    if ((*link_prev= link_next))
      link_next->link_prev= link_prev;
  }
  // Remove slave structure
  for (; slave; slave= slave->next)
    slave->fast_exclude();
  
}

/*
  excluding select_lex structure (except first (first select can't be
  deleted, because it is most upper select))
*/
void st_select_lex_node::exclude()
{
  //exclude from global list
  fast_exclude();
  //exclude from other structures
  if ((*prev= next))
    next->prev= prev;
  /* 
     We do not need following statements, because prev pointer of first 
     list element point to master->slave
     if (master->slave == this)
       master->slave= next;
  */
}


/*
  Exclude level of current unit from tree of SELECTs

  SYNOPSYS
    st_select_lex_unit::exclude_level()

  NOTE: units which belong to current will be brought up on level of
  currernt unit 
*/
void st_select_lex_unit::exclude_level()
{
  SELECT_LEX_UNIT *units= 0, **units_last= &units;
  for (SELECT_LEX *sl= first_select(); sl; sl= sl->next_select())
  {
    // unlink current level from global SELECTs list
    if (sl->link_prev && (*sl->link_prev= sl->link_next))
      sl->link_next->link_prev= sl->link_prev;

    // bring up underlay levels
    SELECT_LEX_UNIT **last= 0;
    for (SELECT_LEX_UNIT *u= sl->first_inner_unit(); u; u= u->next_unit())
    {
      u->master= master;
      last= (SELECT_LEX_UNIT**)&(u->next);
    }
    if (last)
    {
      (*units_last)= sl->first_inner_unit();
      units_last= last;
    }
  }
  if (units)
  {
    // include brought up levels in place of current
    (*prev)= units;
    (*units_last)= (SELECT_LEX_UNIT*)next;
    if (next)
      next->prev= (SELECT_LEX_NODE**)units_last;
    units->prev= prev;
  }
  else
  {
    // exclude currect unit from list of nodes
    (*prev)= next;
    if (next)
      next->prev= prev;
  }
}


/*
  Exclude subtree of current unit from tree of SELECTs

  SYNOPSYS
    st_select_lex_unit::exclude_tree()
*/
void st_select_lex_unit::exclude_tree()
{
  for (SELECT_LEX *sl= first_select(); sl; sl= sl->next_select())
  {
    // unlink current level from global SELECTs list
    if (sl->link_prev && (*sl->link_prev= sl->link_next))
      sl->link_next->link_prev= sl->link_prev;

    // unlink underlay levels
    for (SELECT_LEX_UNIT *u= sl->first_inner_unit(); u; u= u->next_unit())
    {
      u->exclude_level();
    }
  }
  // exclude currect unit from list of nodes
  (*prev)= next;
  if (next)
    next->prev= prev;
}


/*
  st_select_lex_node::mark_as_dependent mark all st_select_lex struct from 
  this to 'last' as dependent

  SYNOPSIS
    last - pointer to last st_select_lex struct, before wich all 
           st_select_lex have to be marked as dependent

  NOTE
    'last' should be reachable from this st_select_lex_node
*/

void st_select_lex::mark_as_dependent(st_select_lex *last)
{
  /*
    Mark all selects from resolved to 1 before select where was
    found table as depended (of select where was found table)
  */
  for (SELECT_LEX *s= this;
       s && s != last;
       s= s->outer_select())
    if (!(s->uncacheable & UNCACHEABLE_DEPENDENT))
    {
      // Select is dependent of outer select
      s->uncacheable= (s->uncacheable & ~UNCACHEABLE_UNITED) |
                       UNCACHEABLE_DEPENDENT;
      SELECT_LEX_UNIT *munit= s->master_unit();
      munit->uncacheable= (munit->uncacheable & ~UNCACHEABLE_UNITED) |
                       UNCACHEABLE_DEPENDENT;
      for (SELECT_LEX *sl= munit->first_select(); sl ; sl= sl->next_select())
      {
        if (sl != s &&
            !(sl->uncacheable & (UNCACHEABLE_DEPENDENT | UNCACHEABLE_UNITED)))
          sl->uncacheable|= UNCACHEABLE_UNITED;
      }
    }
  is_correlated= TRUE;
  this->master_unit()->item->is_correlated= TRUE;
}

bool st_select_lex_node::set_braces(bool value)      { return 1; }
bool st_select_lex_node::inc_in_sum_expr()           { return 1; }
uint st_select_lex_node::get_in_sum_expr()           { return 0; }
TABLE_LIST* st_select_lex_node::get_table_list()     { return 0; }
List<Item>* st_select_lex_node::get_item_list()      { return 0; }
TABLE_LIST *st_select_lex_node::add_table_to_list (THD *thd, Table_ident *table,
						  LEX_STRING *alias,
						  ulong table_join_options,
						  thr_lock_type flags,
						  List<Index_hint> *hints,
                                                  LEX_STRING *option)
{
  return 0;
}
ulong st_select_lex_node::get_table_join_options()
{
  return 0;
}

/*
  prohibit using LIMIT clause
*/
bool st_select_lex::test_limit()
{
  if (select_limit != 0)
  {
    my_error(ER_NOT_SUPPORTED_YET, MYF(0),
             "LIMIT & IN/ALL/ANY/SOME subquery");
    return(1);
  }
  return(0);
}


st_select_lex_unit* st_select_lex_unit::master_unit()
{
    return this;
}


st_select_lex* st_select_lex_unit::outer_select()
{
  return (st_select_lex*) master;
}


bool st_select_lex::add_order_to_list(THD *thd, Item *item, bool asc)
{
  return add_to_list(thd, order_list, item, asc);
}


bool st_select_lex::add_item_to_list(THD *thd, Item *item)
{
  DBUG_ENTER("st_select_lex::add_item_to_list");
  DBUG_PRINT("info", ("Item: 0x%lx", (long) item));
  DBUG_RETURN(item_list.push_back(item));
}


bool st_select_lex::add_group_to_list(THD *thd, Item *item, bool asc)
{
  return add_to_list(thd, group_list, item, asc);
}


bool st_select_lex::add_ftfunc_to_list(Item_func_match *func)
{
  return !func || ftfunc_list->push_back(func); // end of memory?
}


st_select_lex_unit* st_select_lex::master_unit()
{
  return (st_select_lex_unit*) master;
}


st_select_lex* st_select_lex::outer_select()
{
  return (st_select_lex*) master->get_master();
}


bool st_select_lex::set_braces(bool value)
{
  braces= value;
  return 0; 
}


bool st_select_lex::inc_in_sum_expr()
{
  in_sum_expr++;
  return 0;
}


uint st_select_lex::get_in_sum_expr()
{
  return in_sum_expr;
}


TABLE_LIST* st_select_lex::get_table_list()
{
  return table_list.first;
}

List<Item>* st_select_lex::get_item_list()
{
  return &item_list;
}

ulong st_select_lex::get_table_join_options()
{
  return table_join_options;
}


bool st_select_lex::setup_ref_array(THD *thd, uint order_group_num)
{
  if (ref_pointer_array)
    return 0;

  // find_order_in_list() may need some extra space, so multiply by two.
  order_group_num*= 2;

  /*
    We have to create array in prepared statement memory if it is
    prepared statement
  */
  Query_arena *arena= thd->stmt_arena;
  return (ref_pointer_array=
          (Item **)arena->alloc(sizeof(Item*) * (n_child_sum_items +
                                                 item_list.elements +
                                                 select_n_having_items +
                                                 select_n_where_fields +
                                                 order_group_num)*5)) == 0;
}


void st_select_lex_unit::print(String *str, enum_query_type query_type)
{
  bool union_all= !union_distinct;
  for (SELECT_LEX *sl= first_select(); sl; sl= sl->next_select())
  {
    if (sl != first_select())
    {
      str->append(STRING_WITH_LEN(" union "));
      if (union_all)
	str->append(STRING_WITH_LEN("all "));
      else if (union_distinct == sl)
        union_all= TRUE;
    }
    if (sl->braces)
      str->append('(');
    sl->print(thd, str, query_type);
    if (sl->braces)
      str->append(')');
  }
  if (fake_select_lex == global_parameters)
  {
    if (fake_select_lex->order_list.elements)
    {
      str->append(STRING_WITH_LEN(" order by "));
      fake_select_lex->print_order(str,
        fake_select_lex->order_list.first,
        query_type);
    }
    fake_select_lex->print_limit(thd, str, query_type);
  }
}


void st_select_lex::print_order(String *str,
                                ORDER *order,
                                enum_query_type query_type)
{
  for (; order; order= order->next)
  {
    if (order->counter_used)
    {
      char buffer[20];
      size_t length= my_snprintf(buffer, 20, "%d", order->counter);
      str->append(buffer, (uint) length);
    }
    else
      (*order->item)->print(str, query_type);
    if (!order->asc)
      str->append(STRING_WITH_LEN(" desc"));
    if (order->next)
      str->append(',');
  }
}
 

void st_select_lex::print_limit(THD *thd,
                                String *str,
                                enum_query_type query_type)
{
  SELECT_LEX_UNIT *unit= master_unit();
  Item_subselect *item= unit->item;
  if (item && unit->global_parameters == this &&
      (item->substype() == Item_subselect::EXISTS_SUBS ||
       item->substype() == Item_subselect::IN_SUBS ||
       item->substype() == Item_subselect::ALL_SUBS))
  {
    DBUG_ASSERT(!item->fixed ||
                (select_limit->val_int() == LL(1) && offset_limit == 0));
    return;
  }

  if (explicit_limit)
  {
    str->append(STRING_WITH_LEN(" limit "));
    if (offset_limit)
    {
      offset_limit->print(str, query_type);
      str->append(',');
    }
    select_limit->print(str, query_type);
  }
}

/**
  @brief Restore the LEX and THD in case of a parse error.

  This is a clean up call that is invoked by the Bison generated
  parser before returning an error from MYSQLparse. If your
  semantic actions manipulate with the global thread state (which
  is a very bad practice and should not normally be employed) and
  need a clean-up in case of error, and you can not use %destructor
  rule in the grammar file itself, this function should be used
  to implement the clean up.
*/

void st_lex::cleanup_lex_after_parse_error(THD *thd)
{
  /*
    Delete sphead for the side effect of restoring of the original
    LEX state, thd->lex, thd->mem_root and thd->free_list if they
    were replaced when parsing stored procedure statements.  We
    will never use sphead object after a parse error, so it's okay
    to delete it only for the sake of the side effect.
    TODO: make this functionality explicit in sp_head class.
    Sic: we must nullify the member of the main lex, not the
    current one that will be thrown away
  */
  if (thd->lex->sphead)
  {
    thd->lex->sphead->restore_thd_mem_root(thd);
    delete thd->lex->sphead;
    thd->lex->sphead= NULL;
  }
}

/*
  Initialize (or reset) Query_tables_list object.

  SYNOPSIS
    reset_query_tables_list()
      init  TRUE  - we should perform full initialization of object with
                    allocating needed memory
            FALSE - object is already initialized so we should only reset
                    its state so it can be used for parsing/processing
                    of new statement

  DESCRIPTION
    This method initializes Query_tables_list so it can be used as part
    of LEX object for parsing/processing of statement. One can also use
    this method to reset state of already initialized Query_tables_list
    so it can be used for processing of new statement.
*/

void Query_tables_list::reset_query_tables_list(bool init)
{
  if (!init && query_tables)
  {
    TABLE_LIST *table= query_tables;
    for (;;)
    {
      delete table->view;
      if (query_tables_last == &table->next_global ||
          !(table= table->next_global))
        break;
    }
  }
  query_tables= 0;
  query_tables_last= &query_tables;
  query_tables_own_last= 0;
  if (init)
  {
    /*
      We delay real initialization of hash (and therefore related
      memory allocation) until first insertion into this hash.
    */
    hash_clear(&sroutines);
  }
  else if (sroutines.records)
  {
    /* Non-zero sroutines.records means that hash was initialized. */
    my_hash_reset(&sroutines);
  }
  sroutines_list.empty();
  sroutines_list_own_last= sroutines_list.next;
  sroutines_list_own_elements= 0;
  binlog_stmt_flags= 0;
}


/*
  Destroy Query_tables_list object with freeing all resources used by it.

  SYNOPSIS
    destroy_query_tables_list()
*/

void Query_tables_list::destroy_query_tables_list()
{
  hash_free(&sroutines);
}


/*
  Initialize LEX object.

  SYNOPSIS
    st_lex::st_lex()

  NOTE
    LEX object initialized with this constructor can be used as part of
    THD object for which one can safely call open_tables(), lock_tables()
    and close_thread_tables() functions. But it is not yet ready for
    statement parsing. On should use lex_start() function to prepare LEX
    for this.
*/

st_lex::st_lex()
  :result(0),
   sql_command(SQLCOM_END), option_type(OPT_DEFAULT), is_lex_started(0)
{

  my_init_dynamic_array2(&plugins, sizeof(plugin_ref),
                         plugins_static_buffer,
                         INITIAL_LEX_PLUGIN_LIST_SIZE, 
                         INITIAL_LEX_PLUGIN_LIST_SIZE);
  reset_query_tables_list(TRUE);
}


/*
  Check whether the merging algorithm can be used on this VIEW

  SYNOPSIS
    st_lex::can_be_merged()

  DESCRIPTION
    We can apply merge algorithm if it is single SELECT view  with
    subqueries only in WHERE clause (we do not count SELECTs of underlying
    views, and second level subqueries) and we have not grpouping, ordering,
    HAVING clause, aggregate functions, DISTINCT clause, LIMIT clause and
    several underlying tables.

  RETURN
    FALSE - only temporary table algorithm can be used
    TRUE  - merge algorithm can be used
*/

bool st_lex::can_be_merged()
{
  // TODO: do not forget implement case when select_lex.table_list.elements==0

  /* find non VIEW subqueries/unions */
  bool selects_allow_merge= select_lex.next_select() == 0;
  if (selects_allow_merge)
  {
    for (SELECT_LEX_UNIT *tmp_unit= select_lex.first_inner_unit();
         tmp_unit;
         tmp_unit= tmp_unit->next_unit())
    {
      if (tmp_unit->first_select()->parent_lex == this &&
          (tmp_unit->item == 0 ||
           (tmp_unit->item->place() != IN_WHERE &&
            tmp_unit->item->place() != IN_ON)))
      {
        selects_allow_merge= 0;
        break;
      }
    }
  }

  return (selects_allow_merge &&
	  select_lex.group_list.elements == 0 &&
	  select_lex.having == 0 &&
          select_lex.with_sum_func == 0 &&
	  select_lex.table_list.elements >= 1 &&
	  !(select_lex.options & SELECT_DISTINCT) &&
          select_lex.select_limit == 0);
}


/*
  check if command can use VIEW with MERGE algorithm (for top VIEWs)

  SYNOPSIS
    st_lex::can_use_merged()

  DESCRIPTION
    Only listed here commands can use merge algorithm in top level
    SELECT_LEX (for subqueries will be used merge algorithm if
    st_lex::can_not_use_merged() is not TRUE).

  RETURN
    FALSE - command can't use merged VIEWs
    TRUE  - VIEWs with MERGE algorithms can be used
*/

bool st_lex::can_use_merged()
{
  switch (sql_command)
  {
  case SQLCOM_SELECT:
  case SQLCOM_CREATE_TABLE:
  case SQLCOM_UPDATE:
  case SQLCOM_UPDATE_MULTI:
  case SQLCOM_DELETE:
  case SQLCOM_DELETE_MULTI:
  case SQLCOM_INSERT:
  case SQLCOM_INSERT_SELECT:
  case SQLCOM_REPLACE:
  case SQLCOM_REPLACE_SELECT:
  case SQLCOM_LOAD:
    return TRUE;
  default:
    return FALSE;
  }
}

/*
  Check if command can't use merged views in any part of command

  SYNOPSIS
    st_lex::can_not_use_merged()

  DESCRIPTION
    Temporary table algorithm will be used on all SELECT levels for queries
    listed here (see also st_lex::can_use_merged()).

  RETURN
    FALSE - command can't use merged VIEWs
    TRUE  - VIEWs with MERGE algorithms can be used
*/

bool st_lex::can_not_use_merged()
{
  switch (sql_command)
  {
  case SQLCOM_CREATE_VIEW:
  case SQLCOM_SHOW_CREATE:
  /*
    SQLCOM_SHOW_FIELDS is necessary to make 
    information schema tables working correctly with views.
    see get_schema_tables_result function
  */
  case SQLCOM_SHOW_FIELDS:
    return TRUE;
  default:
    return FALSE;
  }
}

/*
  Detect that we need only table structure of derived table/view

  SYNOPSIS
    only_view_structure()

  RETURN
    TRUE yes, we need only structure
    FALSE no, we need data
*/

bool st_lex::only_view_structure()
{
  switch (sql_command) {
  case SQLCOM_SHOW_CREATE:
  case SQLCOM_SHOW_TABLES:
  case SQLCOM_SHOW_FIELDS:
  case SQLCOM_REVOKE_ALL:
  case SQLCOM_REVOKE:
  case SQLCOM_GRANT:
  case SQLCOM_CREATE_VIEW:
    return TRUE;
  default:
    return FALSE;
  }
}


/*
  Should Items_ident be printed correctly

  SYNOPSIS
    need_correct_ident()

  RETURN
    TRUE yes, we need only structure
    FALSE no, we need data
*/


bool st_lex::need_correct_ident()
{
  switch(sql_command)
  {
  case SQLCOM_SHOW_CREATE:
  case SQLCOM_SHOW_TABLES:
  case SQLCOM_CREATE_VIEW:
    return TRUE;
  default:
    return FALSE;
  }
}

/*
  Get effective type of CHECK OPTION for given view

  SYNOPSIS
    get_effective_with_check()
    view    given view

  NOTE
    It have not sense to set CHECK OPTION for SELECT satement or subqueries,
    so we do not.

  RETURN
    VIEW_CHECK_NONE      no need CHECK OPTION
    VIEW_CHECK_LOCAL     CHECK OPTION LOCAL
    VIEW_CHECK_CASCADED  CHECK OPTION CASCADED
*/

uint8 st_lex::get_effective_with_check(TABLE_LIST *view)
{
  if (view->select_lex->master_unit() == &unit &&
      which_check_option_applicable())
    return (uint8)view->with_check;
  return VIEW_CHECK_NONE;
}


/**
  This method should be called only during parsing.
  It is aware of compound statements (stored routine bodies)
  and will initialize the destination with the default
  database of the stored routine, rather than the default
  database of the connection it is parsed in.
  E.g. if one has no current database selected, or current database 
  set to 'bar' and then issues:

  CREATE PROCEDURE foo.p1() BEGIN SELECT * FROM t1 END//

  t1 is meant to refer to foo.t1, not to bar.t1.

  This method is needed to support this rule.

  @return TRUE in case of error (parsing should be aborted, FALSE in
  case of success
*/

bool
st_lex::copy_db_to(char **p_db, size_t *p_db_length) const
{
  if (sphead)
  {
    DBUG_ASSERT(sphead->m_db.str && sphead->m_db.length);
    /*
      It is safe to assign the string by-pointer, both sphead and
      its statements reside in the same memory root.
    */
    *p_db= sphead->m_db.str;
    if (p_db_length)
      *p_db_length= sphead->m_db.length;
    return FALSE;
  }
  return thd->copy_db_to(p_db, p_db_length);
}

/*
  initialize limit counters

  SYNOPSIS
    st_select_lex_unit::set_limit()
    values	- SELECT_LEX with initial values for counters
*/

void st_select_lex_unit::set_limit(st_select_lex *sl)
{
  ha_rows select_limit_val;
  ulonglong val;

  DBUG_ASSERT(! thd->stmt_arena->is_stmt_prepare());
  val= sl->select_limit ? sl->select_limit->val_uint() : HA_POS_ERROR;
  select_limit_val= (ha_rows)val;
#ifndef BIG_TABLES
  /*
    Check for overflow : ha_rows can be smaller then ulonglong if
    BIG_TABLES is off.
    */
  if (val != (ulonglong)select_limit_val)
    select_limit_val= HA_POS_ERROR;
#endif
  val= sl->offset_limit ? sl->offset_limit->val_uint() : ULL(0);
  offset_limit_cnt= (ha_rows)val;
#ifndef BIG_TABLES
  /* Check for truncation. */
  if (val != (ulonglong)offset_limit_cnt)
    offset_limit_cnt= HA_POS_ERROR;
#endif
  select_limit_cnt= select_limit_val + offset_limit_cnt;
  if (select_limit_cnt < select_limit_val)
    select_limit_cnt= HA_POS_ERROR;		// no limit
}


/**
  @brief Set the initial purpose of this TABLE_LIST object in the list of used
    tables.

  We need to track this information on table-by-table basis, since when this
  table becomes an element of the pre-locked list, it's impossible to identify
  which SQL sub-statement it has been originally used in.

  E.g.:

  User request:                 SELECT * FROM t1 WHERE f1();
  FUNCTION f1():                DELETE FROM t2; RETURN 1;
  BEFORE DELETE trigger on t2:  INSERT INTO t3 VALUES (old.a);

  For this user request, the pre-locked list will contain t1, t2, t3
  table elements, each needed for different DML.

  The trigger event map is updated to reflect INSERT, UPDATE, DELETE,
  REPLACE, LOAD DATA, CREATE TABLE .. SELECT, CREATE TABLE ..
  REPLACE SELECT statements, and additionally ON DUPLICATE KEY UPDATE
  clause.
*/

void st_lex::set_trg_event_type_for_tables()
{
  uint8 new_trg_event_map= 0;

  /*
    Some auxiliary operations
    (e.g. GRANT processing) create TABLE_LIST instances outside
    the parser. Additionally, some commands (e.g. OPTIMIZE) change
    the lock type for a table only after parsing is done. Luckily,
    these do not fire triggers and do not need to pre-load them.
    For these TABLE_LISTs set_trg_event_type is never called, and
    trg_event_map is always empty. That means that the pre-locking
    algorithm will ignore triggers defined on these tables, if
    any, and the execution will either fail with an assert in
    sql_trigger.cc or with an error that a used table was not
    pre-locked, in case of a production build.

    TODO: this usage pattern creates unnecessary module dependencies
    and should be rewritten to go through the parser.
    Table list instances created outside the parser in most cases
    refer to mysql.* system tables. It is not allowed to have
    a trigger on a system table, but keeping track of
    initialization provides extra safety in case this limitation
    is circumvented.
  */

  switch (sql_command) {
  case SQLCOM_LOCK_TABLES:
  /*
    On a LOCK TABLE, all triggers must be pre-loaded for this TABLE_LIST
    when opening an associated TABLE.
  */
    new_trg_event_map= static_cast<uint8>
                        (1 << static_cast<int>(TRG_EVENT_INSERT)) |
                      static_cast<uint8>
                        (1 << static_cast<int>(TRG_EVENT_UPDATE)) |
                      static_cast<uint8>
                        (1 << static_cast<int>(TRG_EVENT_DELETE));
    break;
  /*
    Basic INSERT. If there is an additional ON DUPLIATE KEY UPDATE
    clause, it will be handled later in this method.
  */
  case SQLCOM_INSERT:                           /* fall through */
  case SQLCOM_INSERT_SELECT:
  /*
    LOAD DATA ... INFILE is expected to fire BEFORE/AFTER INSERT
    triggers.
    If the statement also has REPLACE clause, it will be
    handled later in this method.
  */
  case SQLCOM_LOAD:                             /* fall through */
  /*
    REPLACE is semantically equivalent to INSERT. In case
    of a primary or unique key conflict, it deletes the old
    record and inserts a new one. So we also may need to
    fire ON DELETE triggers. This functionality is handled
    later in this method.
  */
  case SQLCOM_REPLACE:                          /* fall through */
  case SQLCOM_REPLACE_SELECT:
  /*
    CREATE TABLE ... SELECT defaults to INSERT if the table or
    view already exists. REPLACE option of CREATE TABLE ...
    REPLACE SELECT is handled later in this method.
  */
  case SQLCOM_CREATE_TABLE:
    new_trg_event_map|= static_cast<uint8>
                          (1 << static_cast<int>(TRG_EVENT_INSERT));
    break;
  /* Basic update and multi-update */
  case SQLCOM_UPDATE:                           /* fall through */
  case SQLCOM_UPDATE_MULTI:
    new_trg_event_map|= static_cast<uint8>
                          (1 << static_cast<int>(TRG_EVENT_UPDATE));
    break;
  /* Basic delete and multi-delete */
  case SQLCOM_DELETE:                           /* fall through */
  case SQLCOM_DELETE_MULTI:
    new_trg_event_map|= static_cast<uint8>
                          (1 << static_cast<int>(TRG_EVENT_DELETE));
    break;
  default:
    break;
  }

  switch (duplicates) {
  case DUP_UPDATE:
    new_trg_event_map|= static_cast<uint8>
                          (1 << static_cast<int>(TRG_EVENT_UPDATE));
    break;
  case DUP_REPLACE:
    new_trg_event_map|= static_cast<uint8>
                          (1 << static_cast<int>(TRG_EVENT_DELETE));
    break;
  case DUP_ERROR:
  default:
    break;
  }


  /*
    Do not iterate over sub-selects, only the tables in the outermost
    SELECT_LEX can be modified, if any.
  */
  TABLE_LIST *tables= select_lex.get_table_list();

  while (tables)
  {
    /*
      This is a fast check to filter out statements that do
      not change data, or tables  on the right side, in case of
      INSERT .. SELECT, CREATE TABLE .. SELECT and so on.
      Here we also filter out OPTIMIZE statement and non-updateable
      views, for which lock_type is TL_UNLOCK or TL_READ after
      parsing.
    */
    if (static_cast<int>(tables->lock_type) >=
        static_cast<int>(TL_WRITE_ALLOW_WRITE))
      tables->trg_event_map= new_trg_event_map;
    tables= tables->next_local;
  }
}


/*
  Unlink the first table from the global table list and the first table from
  outer select (lex->select_lex) local list

  SYNOPSIS
    unlink_first_table()
    link_to_local	Set to 1 if caller should link this table to local list

  NOTES
    We assume that first tables in both lists is the same table or the local
    list is empty.

  RETURN
    0	If 'query_tables' == 0
    unlinked table
      In this case link_to_local is set.

*/
TABLE_LIST *st_lex::unlink_first_table(bool *link_to_local)
{
  TABLE_LIST *first;
  if ((first= query_tables))
  {
    /*
      Exclude from global table list
    */
    if ((query_tables= query_tables->next_global))
      query_tables->prev_global= &query_tables;
    else
      query_tables_last= &query_tables;
    first->next_global= 0;

    /*
      and from local list if it is not empty
    */
    if ((*link_to_local= test(select_lex.table_list.first)))
    {
      select_lex.context.table_list= 
        select_lex.context.first_name_resolution_table= first->next_local;
      select_lex.table_list.first= first->next_local;
      select_lex.table_list.elements--;	//safety
      first->next_local= 0;
      /*
        Ensure that the global list has the same first table as the local
        list.
      */
      first_lists_tables_same();
    }
  }
  return first;
}


/*
  Bring first local table of first most outer select to first place in global
  table list

  SYNOPSYS
     st_lex::first_lists_tables_same()

  NOTES
    In many cases (for example, usual INSERT/DELETE/...) the first table of
    main SELECT_LEX have special meaning => check that it is the first table
    in global list and re-link to be first in the global list if it is
    necessary.  We need such re-linking only for queries with sub-queries in
    the select list, as only in this case tables of sub-queries will go to
    the global list first.
*/

void st_lex::first_lists_tables_same()
{
  TABLE_LIST *first_table= select_lex.table_list.first;
  if (query_tables != first_table && first_table != 0)
  {
    TABLE_LIST *next;
    if (query_tables_last == &first_table->next_global)
      query_tables_last= first_table->prev_global;

    if ((next= *first_table->prev_global= first_table->next_global))
      next->prev_global= first_table->prev_global;
    /* include in new place */
    first_table->next_global= query_tables;
    /*
       We are sure that query_tables is not 0, because first_table was not
       first table in the global list => we can use
       query_tables->prev_global without check of query_tables
    */
    query_tables->prev_global= &first_table->next_global;
    first_table->prev_global= &query_tables;
    query_tables= first_table;
  }
}


/*
  Link table back that was unlinked with unlink_first_table()

  SYNOPSIS
    link_first_table_back()
    link_to_local	do we need link this table to local

  RETURN
    global list
*/

void st_lex::link_first_table_back(TABLE_LIST *first,
				   bool link_to_local)
{
  if (first)
  {
    if ((first->next_global= query_tables))
      query_tables->prev_global= &first->next_global;
    else
      query_tables_last= &first->next_global;
    query_tables= first;

    if (link_to_local)
    {
      first->next_local= select_lex.table_list.first;
      select_lex.context.table_list= first;
      select_lex.table_list.first= first;
      select_lex.table_list.elements++;	//safety
    }
  }
}



/*
  cleanup lex for case when we open table by table for processing

  SYNOPSIS
    st_lex::cleanup_after_one_table_open()

  NOTE
    This method is mostly responsible for cleaning up of selects lists and
    derived tables state. To rollback changes in Query_tables_list one has
    to call Query_tables_list::reset_query_tables_list(FALSE).
*/

void st_lex::cleanup_after_one_table_open()
{
  /*
    thd->lex->derived_tables & additional units may be set if we open
    a view. It is necessary to clear thd->lex->derived_tables flag
    to prevent processing of derived tables during next open_and_lock_tables
    if next table is a real table and cleanup & remove underlying units
    NOTE: all units will be connected to thd->lex->select_lex, because we
    have not UNION on most upper level.
    */
  if (all_selects_list != &select_lex)
  {
    derived_tables= 0;
    /* cleunup underlying units (units of VIEW) */
    for (SELECT_LEX_UNIT *un= select_lex.first_inner_unit();
         un;
         un= un->next_unit())
      un->cleanup();
    /* reduce all selects list to default state */
    all_selects_list= &select_lex;
    /* remove underlying units (units of VIEW) subtree */
    select_lex.cut_subtree();
  }
}


/*
  Save current state of Query_tables_list for this LEX, and prepare it
  for processing of new statemnt.

  SYNOPSIS
    reset_n_backup_query_tables_list()
      backup  Pointer to Query_tables_list instance to be used for backup
*/

void st_lex::reset_n_backup_query_tables_list(Query_tables_list *backup)
{
  backup->set_query_tables_list(this);
  /*
    We have to perform full initialization here since otherwise we
    will damage backed up state.
  */
  this->reset_query_tables_list(TRUE);
}


/*
  Restore state of Query_tables_list for this LEX from backup.

  SYNOPSIS
    restore_backup_query_tables_list()
      backup  Pointer to Query_tables_list instance used for backup
*/

void st_lex::restore_backup_query_tables_list(Query_tables_list *backup)
{
  this->destroy_query_tables_list();
  this->set_query_tables_list(backup);
}


/*
  Checks for usage of routines and/or tables in a parsed statement

  SYNOPSIS
    st_lex:table_or_sp_used()

  RETURN
    FALSE  No routines and tables used
    TRUE   Either or both routines and tables are used.
*/

bool st_lex::table_or_sp_used()
{
  DBUG_ENTER("table_or_sp_used");

  if (sroutines.records || query_tables)
    DBUG_RETURN(TRUE);

  DBUG_RETURN(FALSE);
}


/*
  Do end-of-prepare fixup for list of tables and their merge-VIEWed tables

  SYNOPSIS
    fix_prepare_info_in_table_list()
      thd  Thread handle
      tbl  List of tables to process

  DESCRIPTION
    Perform end-end-of prepare fixup for list of tables, if any of the tables
    is a merge-algorithm VIEW, recursively fix up its underlying tables as
    well.

*/

static void fix_prepare_info_in_table_list(THD *thd, TABLE_LIST *tbl)
{
  for (; tbl; tbl= tbl->next_local)
  {
    if (tbl->on_expr)
    {
      tbl->prep_on_expr= tbl->on_expr;
      tbl->on_expr= tbl->on_expr->copy_andor_structure(thd);
    }
    fix_prepare_info_in_table_list(thd, tbl->merge_underlying_list);
  }
}


/*
  Save WHERE/HAVING/ON clauses and replace them with disposable copies

  SYNOPSIS
    st_select_lex::fix_prepare_information
      thd          thread handler
      conds        in/out pointer to WHERE condition to be met at execution
      having_conds in/out pointer to HAVING condition to be met at execution
  
  DESCRIPTION
    The passed WHERE and HAVING are to be saved for the future executions.
    This function saves it, and returns a copy which can be thrashed during
    this execution of the statement. By saving/thrashing here we mean only
    We also save the chain of ORDER::next in group_list, in case
    the list is modified by remove_const().
    AND/OR trees.
    The function also calls fix_prepare_info_in_table_list that saves all
    ON expressions.    
*/

void st_select_lex::fix_prepare_information(THD *thd, Item **conds, 
                                            Item **having_conds)
{
  if (!thd->stmt_arena->is_conventional() && first_execution)
  {
    first_execution= 0;
    if (group_list.first)
    {
      if (!group_list_ptrs)
      {
        void *mem= thd->stmt_arena->alloc(sizeof(Group_list_ptrs));
        group_list_ptrs= new (mem) Group_list_ptrs(thd->stmt_arena->mem_root);
      }
      group_list_ptrs->reserve(group_list.elements);
      for (ORDER *order= group_list.first; order; order= order->next)
      {
        group_list_ptrs->push_back(order);
      }
    }
    if (*conds)
    {
      prep_where= *conds;
      *conds= where= prep_where->copy_andor_structure(thd);
    }
    if (*having_conds)
    {
      prep_having= *having_conds;
      *having_conds= having= prep_having->copy_andor_structure(thd);
    }
    fix_prepare_info_in_table_list(thd, table_list.first);
  }
}


/*
  There are st_select_lex::add_table_to_list &
  st_select_lex::set_lock_for_tables are in sql_parse.cc

  st_select_lex::print is in sql_select.cc

  st_select_lex_unit::prepare, st_select_lex_unit::exec,
  st_select_lex_unit::cleanup, st_select_lex_unit::reinit_exec_mechanism,
  st_select_lex_unit::change_result
  are in sql_union.cc
*/

/*
  Sets the kind of hints to be added by the calls to add_index_hint().

  SYNOPSIS
    set_index_hint_type()
      type_arg     The kind of hints to be added from now on.
      clause       The clause to use for hints to be added from now on.

  DESCRIPTION
    Used in filling up the tagged hints list.
    This list is filled by first setting the kind of the hint as a 
    context variable and then adding hints of the current kind.
    Then the context variable index_hint_type can be reset to the
    next hint type.
*/
void st_select_lex::set_index_hint_type(enum index_hint_type type_arg,
                                        index_clause_map clause)
{ 
  current_index_hint_type= type_arg;
  current_index_hint_clause= clause;
}


/*
  Makes an array to store index usage hints (ADD/FORCE/IGNORE INDEX).

  SYNOPSIS
    alloc_index_hints()
      thd         current thread.
*/

void st_select_lex::alloc_index_hints (THD *thd)
{ 
  index_hints= new (thd->mem_root) List<Index_hint>(); 
}



/*
  adds an element to the array storing index usage hints 
  (ADD/FORCE/IGNORE INDEX).

  SYNOPSIS
    add_index_hint()
      thd         current thread.
      str         name of the index.
      length      number of characters in str.

  RETURN VALUE
    0 on success, non-zero otherwise
*/
bool st_select_lex::add_index_hint (THD *thd, char *str, uint length)
{
  return index_hints->push_front (new (thd->mem_root) 
                                 Index_hint(current_index_hint_type,
                                            current_index_hint_clause,
                                            str, length));
}

/**
  A routine used by the parser to decide whether we are specifying a full
  partitioning or if only partitions to add or to split.

  @note  This needs to be outside of WITH_PARTITION_STORAGE_ENGINE since it
  is used from the sql parser that doesn't have any ifdef's

  @retval  TRUE    Yes, it is part of a management partition command
  @retval  FALSE          No, not a management partition command
*/

bool st_lex::is_partition_management() const
{
  return (sql_command == SQLCOM_ALTER_TABLE &&
          (alter_info.flags == ALTER_ADD_PARTITION ||
           alter_info.flags == ALTER_REORGANIZE_PARTITION));
}

#ifdef HAVE_EXPLICIT_TEMPLATE_INSTANTIATION
template class Mem_root_array<ORDER*, true>;
#endif

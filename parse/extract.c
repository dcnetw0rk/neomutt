/**
 * @file
 * Text parser
 *
 * @authors
 * Copyright (C) 2023 Richard Russon <rich@flatcap.org>
 *
 * @copyright
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 2 of the License, or (at your option) any later
 * version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/**
 * @page parse_extract Text parser
 *
 * Text parser
 */

#include "config.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include "mutt/lib.h"
#include "config/lib.h"
#include "core/lib.h"
#include "extract.h"
#include "myvar.h"

/**
 * parse_extract_token - Extract one token from a string
 * @param dest  Buffer for the result
 * @param tok   Buffer containing tokens
 * @param flags Flags, see #TokenFlags
 * @retval  0 Success
 * @retval -1 Error
 */
int parse_extract_token(struct Buffer *dest, struct Buffer *tok, TokenFlags flags)
{
  if (!dest || !tok)
    return -1;

  char ch;
  char qc = '\0'; /* quote char */
  char *pc = NULL;

  /* Some callers used to rely on the (bad) assumption that dest->data would be
   * non-NULL after calling this function.  Perhaps I've missed a few cases, or
   * a future caller might make the same mistake.  */
  if (!dest->data)
    mutt_buffer_alloc(dest, 256);

  mutt_buffer_reset(dest);

  SKIPWS(tok->dptr);
  while ((ch = *tok->dptr))
  {
    if (qc == '\0')
    {
      if ((IS_SPACE(ch) && !(flags & TOKEN_SPACE)) ||
          ((ch == '#') && !(flags & TOKEN_COMMENT)) ||
          ((ch == '+') && (flags & TOKEN_PLUS)) || ((ch == '-') && (flags & TOKEN_MINUS)) ||
          ((ch == '=') && (flags & TOKEN_EQUAL)) ||
          ((ch == '?') && (flags & TOKEN_QUESTION)) ||
          ((ch == ';') && !(flags & TOKEN_SEMICOLON)) ||
          ((flags & TOKEN_PATTERN) && strchr("~%=!|", ch)))
      {
        break;
      }
    }

    tok->dptr++;

    if (ch == qc)
      qc = 0; /* end of quote */
    else if (!qc && ((ch == '\'') || (ch == '"')) && !(flags & TOKEN_QUOTE))
      qc = ch;
    else if ((ch == '\\') && (qc != '\''))
    {
      if (tok->dptr[0] == '\0')
        return -1; /* premature end of token */
      switch (ch = *tok->dptr++)
      {
        case 'c':
        case 'C':
          if (tok->dptr[0] == '\0')
            return -1; /* premature end of token */
          mutt_buffer_addch(dest, (toupper((unsigned char) tok->dptr[0]) - '@') & 0x7f);
          tok->dptr++;
          break;
        case 'e':
          mutt_buffer_addch(dest, '\033'); // Escape
          break;
        case 'f':
          mutt_buffer_addch(dest, '\f');
          break;
        case 'n':
          mutt_buffer_addch(dest, '\n');
          break;
        case 'r':
          mutt_buffer_addch(dest, '\r');
          break;
        case 't':
          mutt_buffer_addch(dest, '\t');
          break;
        default:
          if (isdigit((unsigned char) ch) && isdigit((unsigned char) tok->dptr[0]) &&
              isdigit((unsigned char) tok->dptr[1]))
          {
            mutt_buffer_addch(dest, (ch << 6) + (tok->dptr[0] << 3) + tok->dptr[1] - 3504);
            tok->dptr += 2;
          }
          else
          {
            mutt_buffer_addch(dest, ch);
          }
      }
    }
    else if ((ch == '^') && (flags & TOKEN_CONDENSE))
    {
      if (tok->dptr[0] == '\0')
        return -1; /* premature end of token */
      ch = *tok->dptr++;
      if (ch == '^')
        mutt_buffer_addch(dest, ch);
      else if (ch == '[')
        mutt_buffer_addch(dest, '\033'); // Escape
      else if (isalpha((unsigned char) ch))
        mutt_buffer_addch(dest, toupper((unsigned char) ch) - '@');
      else
      {
        mutt_buffer_addch(dest, '^');
        mutt_buffer_addch(dest, ch);
      }
    }
    else if ((ch == '`') && (!qc || (qc == '"')))
    {
      FILE *fp = NULL;
      pid_t pid;

      pc = tok->dptr;
      do
      {
        pc = strpbrk(pc, "\\`");
        if (pc)
        {
          /* skip any quoted chars */
          if (*pc == '\\')
            pc += 2;
        }
      } while (pc && (pc[0] != '`'));
      if (!pc)
      {
        mutt_debug(LL_DEBUG1, "mismatched backticks\n");
        return -1;
      }
      struct Buffer cmd;
      mutt_buffer_init(&cmd);
      *pc = '\0';
      if (flags & TOKEN_BACKTICK_VARS)
      {
        /* recursively extract tokens to interpolate variables */
        parse_extract_token(&cmd, tok,
                            TOKEN_QUOTE | TOKEN_SPACE | TOKEN_COMMENT |
                                TOKEN_SEMICOLON | TOKEN_NOSHELL);
      }
      else
      {
        cmd.data = mutt_str_dup(tok->dptr);
      }
      *pc = '`';
      pid = filter_create(cmd.data, NULL, &fp, NULL);
      if (pid < 0)
      {
        mutt_debug(LL_DEBUG1, "unable to fork command: %s\n", cmd.data);
        FREE(&cmd.data);
        return -1;
      }

      tok->dptr = pc + 1;

      /* read line */
      struct Buffer expn = mutt_buffer_make(0);
      expn.data = mutt_file_read_line(NULL, &expn.dsize, fp, NULL, MUTT_RL_NO_FLAGS);
      mutt_file_fclose(&fp);
      int rc = filter_wait(pid);
      if (rc != 0)
        mutt_debug(LL_DEBUG1, "backticks exited code %d for command: %s\n", rc,
                   mutt_buffer_string(&cmd));
      FREE(&cmd.data);

      /* if we got output, make a new string consisting of the shell output
       * plus whatever else was left on the original line */
      /* BUT: If this is inside a quoted string, directly add output to
       * the token */
      if (expn.data)
      {
        if (qc)
        {
          mutt_buffer_addstr(dest, expn.data);
        }
        else
        {
          struct Buffer *copy = mutt_buffer_pool_get();
          mutt_buffer_fix_dptr(&expn);
          mutt_buffer_copy(copy, &expn);
          mutt_buffer_addstr(copy, tok->dptr);
          mutt_buffer_copy(tok, copy);
          mutt_buffer_seek(tok, 0);
          mutt_buffer_pool_release(&copy);
        }
        FREE(&expn.data);
      }
    }
    else if ((ch == '$') && (!qc || (qc == '"')) &&
             ((tok->dptr[0] == '{') || isalpha((unsigned char) tok->dptr[0])))
    {
      const char *env = NULL;
      char *var = NULL;

      if (tok->dptr[0] == '{')
      {
        pc = strchr(tok->dptr, '}');
        if (pc)
        {
          var = mutt_strn_dup(tok->dptr + 1, pc - (tok->dptr + 1));
          tok->dptr = pc + 1;

          if ((flags & TOKEN_NOSHELL))
          {
            mutt_buffer_addch(dest, ch);
            mutt_buffer_addch(dest, '{');
            mutt_buffer_addstr(dest, var);
            mutt_buffer_addch(dest, '}');
            FREE(&var);
          }
        }
      }
      else
      {
        for (pc = tok->dptr; isalnum((unsigned char) *pc) || (pc[0] == '_'); pc++)
          ; // do nothing

        var = mutt_strn_dup(tok->dptr, pc - tok->dptr);
        tok->dptr = pc;
      }
      if (var)
      {
        struct Buffer result;
        mutt_buffer_init(&result);
        int rc = cs_subset_str_string_get(NeoMutt->sub, var, &result);

        if (CSR_RESULT(rc) == CSR_SUCCESS)
        {
          mutt_buffer_addstr(dest, result.data);
          FREE(&result.data);
        }
        else if ((env = myvar_get(var)))
        {
          mutt_buffer_addstr(dest, env);
        }
        else if (!(flags & TOKEN_NOSHELL) && (env = mutt_str_getenv(var)))
        {
          mutt_buffer_addstr(dest, env);
        }
        else
        {
          mutt_buffer_addch(dest, ch);
          mutt_buffer_addstr(dest, var);
        }
        FREE(&var);
      }
    }
    else
    {
      mutt_buffer_addch(dest, ch);
    }
  }
  mutt_buffer_addch(dest, 0); /* terminate the string */
  SKIPWS(tok->dptr);
  return 0;
}
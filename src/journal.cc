/*
 * Copyright (c) 2003-2010, John Wiegley.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 * - Redistributions of source code must retain the above copyright
 *   notice, this list of conditions and the following disclaimer.
 *
 * - Redistributions in binary form must reproduce the above copyright
 *   notice, this list of conditions and the following disclaimer in the
 *   documentation and/or other materials provided with the distribution.
 *
 * - Neither the name of New Artisans LLC nor the names of its
 *   contributors may be used to endorse or promote products derived from
 *   this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <system.hh>

#include "journal.h"
#include "amount.h"
#include "commodity.h"
#include "pool.h"
#include "xact.h"
#include "post.h"
#include "account.h"

namespace ledger {

journal_t::journal_t()
{
  TRACE_CTOR(journal_t, "");
  initialize();
}

journal_t::journal_t(const path& pathname)
{
  TRACE_CTOR(journal_t, "path");
  initialize();
  read(pathname);
}

journal_t::journal_t(const string& str)
{
  TRACE_CTOR(journal_t, "string");
  initialize();
  read(str);
}

journal_t::~journal_t()
{
  TRACE_DTOR(journal_t);

  // Don't bother unhooking each xact's posts from the accounts they refer to,
  // because all accounts are about to be deleted.
  foreach (xact_t * xact, xacts)
    checked_delete(xact);

  foreach (auto_xact_t * xact, auto_xacts)
    checked_delete(xact);

  foreach (period_xact_t * xact, period_xacts)
    checked_delete(xact);

  checked_delete(master);
}

void journal_t::initialize()
{
  master            = new account_t;
  bucket            = NULL;
  fixed_accounts    = false;
  fixed_payees      = false;
  fixed_commodities = false;
  fixed_metadata    = false;
  was_loaded        = false;
  force_checking    = false;
  checking_style    = CHECK_PERMISSIVE;
}

void journal_t::add_account(account_t * acct)
{
  master->add_account(acct);
}

bool journal_t::remove_account(account_t * acct)
{
  return master->remove_account(acct);
}

account_t * journal_t::find_account(const string& name, bool auto_create)
{
  return master->find_account(name, auto_create);
}

account_t * journal_t::find_account_re(const string& regexp)
{
  return master->find_account_re(regexp);
}

account_t * journal_t::register_account(const string& name, post_t * post,
                                        const string& location,
                                        account_t * master_account)
{
  account_t * result = NULL;

  if (account_aliases.size() > 0) {
    accounts_map::const_iterator i = account_aliases.find(name);
    if (i != account_aliases.end())
      result = (*i).second;
  }
  if (! result)
    result = master_account->find_account(name);

  if (result->name == _("Unknown")) {
    foreach (account_mapping_t& value, payees_for_unknown_accounts) {
      if (value.first.match(post->xact->payee)) {
        result = value.second;
        break;
      }
    }
  }

  if (! result->has_flags(ACCOUNT_KNOWN)) {
    if (! post) {
      if (force_checking)
        fixed_accounts = true;
      result->add_flags(ACCOUNT_KNOWN);
    }
    else if (! fixed_accounts && post->_state != item_t::UNCLEARED) {
      result->add_flags(ACCOUNT_KNOWN);
    }
    else if (checking_style == CHECK_WARNING) {
      warning_(_("%1Unknown account '%2'") << location
               << result->fullname());
    }
    else if (checking_style == CHECK_ERROR) {
      throw_(parse_error, _("Unknown account '%1'") << result->fullname());
    }
  }

  return result;
}

string journal_t::register_payee(const string& name, xact_t *, const string&)
{
  string payee;

#if 0
  std::set<string>::iterator i = known_payees.find(name);

  if (i == known_payees.end()) {
    if (! xact) {
      if (force_checking)
        fixed_payees = true;
      known_payees.insert(name);
    }
    else if (! fixed_payees && xact->_state != item_t::UNCLEARED) {
      known_payees.insert(name);
    }
    else if (checking_style == CHECK_WARNING) {
      warning_(_("%1Unknown payee '%2'") << location << name);
    }
    else if (checking_style == CHECK_ERROR) {
      throw_(parse_error, _("Unknown payee '%1'") << name);
    }
  }
#endif

  foreach (payee_mapping_t& value, payee_mappings) {
    if (value.first.match(name)) {
      payee = value.second;
      break;
    }
  }

  return payee.empty() ? name : payee;
}

void journal_t::register_commodity(commodity_t& comm,
                                   variant<int, xact_t *, post_t *> context,
                                   const string& location)
{
  if (! comm.has_flags(COMMODITY_KNOWN)) {
    if (context.which() == 0) {
      if (force_checking)
        fixed_commodities = true;
      comm.add_flags(COMMODITY_KNOWN);
    }
    else if (! fixed_commodities &&
             ((context.which() == 1 &&
               boost::get<xact_t *>(context)->_state != item_t::UNCLEARED) ||
             (context.which() == 2 &&
               boost::get<post_t *>(context)->_state != item_t::UNCLEARED))) {
      comm.add_flags(COMMODITY_KNOWN);
    }
    else if (checking_style == CHECK_WARNING) {
      warning_(_("%1Unknown commodity '%2'") << location << comm);
    }
    else if (checking_style == CHECK_ERROR) {
      throw_(parse_error, _("Unknown commodity '%1'") << comm);
    }
  }
}

namespace {
  void check_metadata(journal_t& journal,
                      const string& key, const value_t& value,
                      variant<int, xact_t *, post_t *> context,
                      const string& location)
  {
    std::pair<tag_check_exprs_map::iterator,
              tag_check_exprs_map::iterator> range =
      journal.tag_check_exprs.equal_range(key);

    for (tag_check_exprs_map::iterator i = range.first;
         i != range.second;
         ++i) {
      value_scope_t val_scope
        (context.which() == 1 ?
         static_cast<scope_t&>(*boost::get<xact_t *>(context)) :
         static_cast<scope_t&>(*boost::get<post_t *>(context)), value);

      if (! (*i).second.first.calc(val_scope).to_boolean()) {
        if ((*i).second.second == expr_t::EXPR_ASSERTION)
          throw_(parse_error,
                 _("Metadata assertion failed for (%1: %2): %3")
                 << key << value << (*i).second.first);
        else
          warning_(_("%1Metadata check failed for (%2: %3): %4")
                   << location << key << value << (*i).second.first);
      }
    }
  }
}

void journal_t::register_metadata(const string& key, const value_t& value,
                                  variant<int, xact_t *, post_t *> context,
                                  const string& location)
{
  std::set<string>::iterator i = known_tags.find(key);

  if (i == known_tags.end()) {
    if (context.which() == 0) {
      if (force_checking)
        fixed_metadata = true;
      known_tags.insert(key);
    }
    else if (! fixed_metadata &&
             ((context.which() == 1 &&
               boost::get<xact_t *>(context)->_state != item_t::UNCLEARED) ||
             (context.which() == 2 &&
               boost::get<post_t *>(context)->_state != item_t::UNCLEARED))) {
      known_tags.insert(key);
    }
    else if (checking_style == CHECK_WARNING) {
      warning_(_("%1Unknown metadata tag '%2'") << location << key);
    }
    else if (checking_style == CHECK_ERROR) {
      throw_(parse_error, _("Unknown metadata tag '%1'") << key);
    }
  }

  if (! value.is_null())
    check_metadata(*this, key, value, context, location);
}

namespace {
  void check_all_metadata(journal_t& journal,
                          variant<int, xact_t *, post_t *> context)
  {
    xact_t * xact = context.which() == 1 ? boost::get<xact_t *>(context) : NULL;
    post_t * post = context.which() == 2 ? boost::get<post_t *>(context) : NULL;

    if ((xact || post) && xact ? xact->metadata : post->metadata) {
      foreach (const item_t::string_map::value_type& pair,
               xact ? *xact->metadata : *post->metadata) {
        const string& key(pair.first);

        // jww (2012-02-27): We really need to know the parsing context,
        // both here and for the call to warning_ in
        // xact_t::extend_xact.
        if (optional<value_t> value = pair.second.first)
          journal.register_metadata(key, *value, context, "");
        else
          journal.register_metadata(key, NULL_VALUE, context, "");
      }
    }
  }
}

bool journal_t::add_xact(xact_t * xact)
{
  xact->journal = this;

  if (! xact->finalize()) {
    xact->journal = NULL;
    return false;
  }

  extend_xact(xact);

  check_all_metadata(*this, xact);
  foreach (post_t * post, xact->posts)
    check_all_metadata(*this, post);

  // If a transaction with this UUID has already been seen, simply do
  // not add this one to the journal.  However, all automated checks
  // will have been performed by extend_xact, so asserts can still be
  // applied to it.
  if (optional<value_t> ref = xact->get_tag(_("UUID"))) {
    std::pair<checksum_map_t::iterator, bool> result
      = checksum_map.insert(checksum_map_t::value_type(ref->to_string(), xact));
    if (! result.second) {
      // jww (2012-02-27): Confirm that the xact in
      // (*result.first).second is exact match in its significant
      // details to xact.
      xact->journal = NULL;
      return false;
    }
  }

  xacts.push_back(xact);

  return true;
}

void journal_t::extend_xact(xact_base_t * xact)
{
  foreach (auto_xact_t * auto_xact, auto_xacts)
    auto_xact->extend_xact(*xact);
}

bool journal_t::remove_xact(xact_t * xact)
{
  bool found = false;
  xacts_list::iterator i;
  for (i = xacts.begin(); i != xacts.end(); i++)
    if (*i == xact) {
      found = true;
      break;
    }
  if (! found)
    return false;

  xacts.erase(i);
  xact->journal = NULL;

  return true;
}

std::size_t journal_t::read(std::istream& in,
                            const path&   pathname,
                            account_t *   master_alt,
                            scope_t *     scope)
{
  std::size_t count = 0;
  try {
    if (! scope)
      scope = scope_t::default_scope;

    if (! scope)
      throw_(std::runtime_error,
             _("No default scope in which to read journal file '%1'")
             << pathname);

    count = parse(in, *scope, master_alt ? master_alt : master, &pathname);
  }
  catch (...) {
    clear_xdata();
    throw;
  }

  // xdata may have been set for some accounts and transaction due to the use
  // of balance assertions or other calculations performed in valexpr-based
  // posting amounts.
  clear_xdata();

  return count;
}

std::size_t journal_t::read(const path& pathname,
                            account_t * master_account,
                            scope_t *   scope)
{
  path filename = resolve_path(pathname);

  if (! exists(filename))
    throw_(std::runtime_error,
           _("Cannot read journal file %1") << filename);

  ifstream stream(filename);
  std::size_t count = read(stream, filename, master_account, scope);
  if (count > 0)
    sources.push_back(fileinfo_t(filename));
  return count;
}

bool journal_t::has_xdata()
{
  foreach (xact_t * xact, xacts)
    if (xact->has_xdata())
      return true;

  foreach (auto_xact_t * xact, auto_xacts)
    if (xact->has_xdata())
      return true;

  foreach (period_xact_t * xact, period_xacts)
    if (xact->has_xdata())
      return true;

  if (master->has_xdata() || master->children_with_xdata())
    return true;

  return false;
}

void journal_t::clear_xdata()
{
  foreach (xact_t * xact, xacts)
    if (! xact->has_flags(ITEM_TEMP))
      xact->clear_xdata();

  foreach (auto_xact_t * xact, auto_xacts)
    if (! xact->has_flags(ITEM_TEMP))
      xact->clear_xdata();

  foreach (period_xact_t * xact, period_xacts)
    if (! xact->has_flags(ITEM_TEMP))
      xact->clear_xdata();

  master->clear_xdata();
}

bool journal_t::valid() const
{
  if (! master->valid()) {
    DEBUG("ledger.validate", "journal_t: master not valid");
    return false;
  }

  foreach (const xact_t * xact, xacts)
    if (! xact->valid()) {
      DEBUG("ledger.validate", "journal_t: xact not valid");
      return false;
    }

  return true;
}

} // namespace ledger

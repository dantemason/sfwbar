/* This entire file is licensed under GNU General Public License v3.0
 *
 * Copyright 2025- sfwbar maintainers
 */

#include "vm/vm.h"
#include "util/string.h"

GHashTable *vm_func_table;

void vm_func_init ( void )
{
  if(vm_func_table)
    return;
  vm_func_table = g_hash_table_new((GHashFunc)str_nhash,
      (GEqualFunc)str_nequal);
}

vm_function_t *vm_func_lookup ( gchar *name )
{
  vm_function_t *func;

  if( (func = (vm_function_t *)g_hash_table_lookup(vm_func_table, name)) )
    return func;

  func = g_malloc0(sizeof(vm_function_t));
  func->name = g_strdup(name);
  g_hash_table_insert(vm_func_table, func->name, func);

  return func;
}

void vm_func_add ( gchar *name, vm_func_t function, gboolean deterministic )
{
  vm_function_t *func;

  func = vm_func_lookup(name);

  func->ptr.function = function;
  func->flags |= (deterministic? VM_FUNC_DETERMINISTIC : 0);
  expr_dep_trigger(name);
  g_debug("function: registered '%s'", name);
}

void vm_func_add_user ( gchar *name, GBytes *code )
{
  vm_function_t *func;

  func = vm_func_lookup(name);
  func->ptr.code = code;
  func->flags = VM_FUNC_USERDEFINED;
  expr_dep_trigger(name);
  g_debug("function: registered '%s'", name);
}

void vm_func_remove ( gchar *name )
{
  vm_function_t *func;

  if( !(func = g_hash_table_lookup(vm_func_table, name)) )
    return;

  func->ptr.function = NULL;
}

/**
 * Copyright (C) 2013-2016 DaSE .
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * version 2 as published by the Free Software Foundation.
 *
 * @file ob_root_ddl_operator.cpp
 * @brief for operations of table in rootserver
 *
 * modified by Wenghaixing:modify main procedure of create table, only when table is not index or index switch is on ,
 *                         root server will create tablet for table
 *
 * @version CEDAR 0.2 
 * @author wenghaixing <wenghaixing@ecnu.cn>
 * @date  2016_01_24
 */

#include "ob_root_ddl_operator.h"
#include "ob_root_server2.h"
#include "common/ob_range.h"
#include "common/ob_schema_service.h"
#include "common/ob_schema_service_impl.h"

using namespace oceanbase::common;
using namespace oceanbase::rootserver;

ObRootDDLOperator::ObRootDDLOperator():schema_client_(NULL), root_server_(NULL)
{
}

ObRootDDLOperator::~ObRootDDLOperator()
{
}

void ObRootDDLOperator::init(ObRootServer2 * server, ObSchemaService * impl)
{
  root_server_ = server;
  schema_client_ = impl;
}

int ObRootDDLOperator::create_table(const TableSchema & table_schema)
{
  int ret = OB_SUCCESS;
  if (!check_inner_stat())
  {
    ret = OB_ERROR;
    TBSYS_LOG(WARN, "check inner stat failed");
  }
  else
  {
    tbsys::CThreadGuard lock(&mutex_lock_);
    // step 1. allocate and update the table max used table id
    ret = allocate_table_id(const_cast<TableSchema &> (table_schema));
    if (ret != OB_SUCCESS)
    {
      TBSYS_LOG(WARN, "allocate table id failed:name[%s], ret[%d]",
          table_schema.table_name_, ret);
    }
    else
    {
      // step 2. update max used table id
      ret = update_max_table_id(table_schema.table_id_);
      if (ret != OB_SUCCESS)
      {
        TBSYS_LOG(WARN, "update max used table id failed:table_name[%s], table_id[%lu], ret[%d]",
            table_schema.table_name_, table_schema.table_id_, ret);
      }
    }
  }
  //modify wenghaixing [secondary index.static_index]20160119
  if ((OB_SUCCESS == ret && OB_INVALID_ID == table_schema.original_table_id_)
          || (OB_SUCCESS == ret && OB_INVALID_ID != table_schema.original_table_id_ && root_server_->get_config().index_immediate_effect))
  //if (OB_SUCCESS == ret)
  //modify e
  {
    // step 3. select cs for create empty tablet
    ret = create_empty_tablet(table_schema);
    if (ret != OB_SUCCESS)
    {
      TBSYS_LOG(WARN, "create empty tablet failed:table_name[%s], table_id[%lu], ret[%d]",
          table_schema.table_name_, table_schema.table_id_, ret);
    }
  }
  if (OB_SUCCESS == ret)
  {
    // step 4. insert table scheam to inner table
    if (true != insert_schema_table(table_schema))
    {
      ret = OB_ERROR;
      TBSYS_LOG(ERROR, "update schema table failed:table_name[%s], table_id[%lu]",
          table_schema.table_name_, table_schema.table_id_);
    }
    else
    {
      TBSYS_LOG(INFO, "update inner table for schema succ:table_name[%s], table_id[%lu]",
          table_schema.table_name_, table_schema.table_id_);
    }
  }
  //add hushuang[scalable commit]20160710
  if(OB_SUCCESS == ret)
  {
    ObString table_name;
    table_name.assign_ptr(const_cast<char*>(table_schema.table_name_), static_cast<int32_t>(strlen(table_schema.table_name_)));
    uint64_t table_id = OB_INVALID_ID;
    int retry_time = 0;
    while(retry_time < 50)
    {
      if(OB_SUCCESS == (ret = schema_client_->get_table_id(table_name, table_id)))
      {
        break;
      }
      else
      {
        retry_time ++;
        ret = OB_ERROR;
      }
    }
    if(OB_SUCCESS != ret)
    {
      TBSYS_LOG(ERROR, "create table failed!");
    }
  }
  //add e

  return ret;
}

int ObRootDDLOperator::create_empty_tablet(const TableSchema & table_schema)
{
  ObArray<ObServer> cs_list;
  int ret = root_server_->create_empty_tablet(const_cast<TableSchema &>(table_schema), cs_list);
  if (ret != OB_SUCCESS)
  {
    TBSYS_LOG(WARN, "create table tablets failed:table_name[%s], table_id[%lu], ret[%d]",
        table_schema.table_name_, table_schema.table_id_, ret);
  }
  else
  {
    TBSYS_LOG(DEBUG, "create empty tablet succ:table_name[%s], table_id[%lu], cs_count[%ld]",
        table_schema.table_name_, table_schema.table_id_, cs_list.count());
  }
  return ret;
}

bool ObRootDDLOperator::insert_schema_table(const TableSchema & table_schema)
{
  bool succ = false;
  int ret = schema_client_->create_table(table_schema);
  if (ret != OB_SUCCESS)
  {
    TBSYS_LOG(WARN, "insert new table schema failed:ret[%d]", ret);
  }
  else
  {
    succ = true;
  }
  // double check for read the table info for comparing
  ObString table_name;
  size_t len = strlen(table_schema.table_name_);
  ObString old_name((int32_t)len, (int32_t)len, table_schema.table_name_);
  ret = schema_client_->get_table_name(table_schema.table_id_, table_name);
  if (ret != OB_SUCCESS)
  {
    // WARNING: IF go into this path, THEN need client check wether write succ or not
    TBSYS_LOG(ERROR, "get table name failed need manual check:table_name[%s], ret[%d]",
        table_schema.table_name_, ret);
  }
  else if (old_name != table_name)
  {
    succ = false;
    TBSYS_LOG(ERROR, "check schema table name not equal:table_name[%s], schema_name[%.*s]",
        table_schema.table_name_, table_name.length(), table_name.ptr());
  }
  else
  {
    succ = true;
  }
  // TODO check table schema content equal with old schema
  return succ;
}

int ObRootDDLOperator::allocate_table_id(TableSchema & table_schema)
{
  int ret = OB_SUCCESS;
  uint64_t table_id = 0;
  // get max table id from inner table
  if ((ret = schema_client_->get_max_used_table_id(table_id)) != OB_SUCCESS)
  {
    TBSYS_LOG(WARN, "get max table id failed:ret[%d]", ret);
  }
  else if (table_schema.table_id_ == OB_INVALID_ID)
  {
    if (table_id + 1 > OB_APP_MIN_TABLE_ID)
    {
      table_schema.table_id_ = table_id + 1;
      TBSYS_LOG(DEBUG, "modify new table schema succ:table_id[%lu]", table_schema.table_id_);
    }
    else
    {
      ret = OB_ERROR;
      TBSYS_LOG(USER_ERROR, "User table id should in (%lu, MAX), ret[%d]", OB_APP_MIN_TABLE_ID, ret);
    }
  }
  else
  {
    if (table_schema.table_id_ > table_id)
    {
      ret = OB_ERROR;
      TBSYS_LOG(USER_ERROR, "User table id must not bigger than ob_max_used_table_id[%lu], ret[%d]", table_id, ret);
    }
  }
  if (ret == OB_SUCCESS)
  {
    for (int64_t i = 0; i < table_schema.join_info_.count(); i ++)
    {
      table_schema.join_info_.at(i).left_table_id_ = table_schema.table_id_;
      TBSYS_LOG(DEBUG, "table schema join info[%s]", to_cstring(table_schema.join_info_.at(i)));
    }
  }
  return ret;
}

int ObRootDDLOperator::update_max_table_id(const uint64_t table_id)
{
  int ret = OB_SUCCESS;
  uint64_t max_id = 0;
  if ((ret = schema_client_->get_max_used_table_id(max_id)) != OB_SUCCESS)
  {
    TBSYS_LOG(WARN, "get max table id failed:ret[%d]", ret);
  }
  else if (table_id > max_id)
  {
    // update the max table id
    int ret = schema_client_->set_max_used_table_id(table_id);
    if (ret != OB_SUCCESS)
    {
      TBSYS_LOG(WARN, "update table max table id failed:max_id[%lu], ret[%d]",
          table_id + 1, ret);
    }
    else
    {
      uint64_t new_max_id = 0;
      ret = schema_client_->get_max_used_table_id(new_max_id);
      if (ret != OB_SUCCESS)
      {
        TBSYS_LOG(WARN, "double check new max table id failed:max_id[%lu], ret[%d]", table_id, ret);
      }
      else if (new_max_id != table_id)
      {
        ret = OB_INNER_STAT_ERROR;
        TBSYS_LOG(ERROR, "check update max table id check failed:max_id[%lu], read[%lu]",
            table_id, new_max_id);
      }
      else
      {
        TBSYS_LOG(INFO, "check updated max table table id succ:max_id[%lu]", new_max_id);
      }
    }
  }
  return ret;
}

int ObRootDDLOperator::drop_table(const common::ObString & table_name)
{
  int ret = OB_SUCCESS;
  uint64_t table_id = 0;
  if (!check_inner_stat())
  {
    ret = OB_ERROR;
    TBSYS_LOG(WARN, "check inner stat failed");
  }
  else
  {
    // WARNING:can not drop the inner table
    if (delete_schema_table(table_name, table_id) != true)
    {
      ret = OB_ERROR;
      TBSYS_LOG(WARN, "delete table from schema table failed:table_name[%.*s], ret[%d]",
          table_name.length(), table_name.ptr(), ret);
    }
    else
    {
      TBSYS_LOG(INFO, "delete table from inner table succ:table_name[%.*s], table_id[%lu]",
          table_name.length(), table_name.ptr(), table_id);
    }
  }
  // clear the root table for this table items
  if (OB_SUCCESS == ret)
  {
    ret = delete_root_table(table_id);
    if (ret != OB_SUCCESS)
    {
      TBSYS_LOG(WARN, "delete root table failed:table_name[%.*s], table_id[%lu], ret[%d]",
          table_name.length(), table_name.ptr(), table_id, ret);
    }
    else
    {
      TBSYS_LOG(INFO, "delete root table succ:table_name[%.*s], table_id[%lu]",
          table_name.length(), table_name.ptr(), table_id);
    }
  }
  return ret;
}

int ObRootDDLOperator::delete_root_table(const uint64_t table_id)
{
  ObArray<uint64_t> list;
  int ret = list.push_back(table_id);
  if (ret != OB_SUCCESS)
  {
    TBSYS_LOG(WARN, "add table id failed:table_id[%lu], ret[%d]", table_id, ret);
  }
  else
  {
    // delete from root table and write commit log
    ret = root_server_->delete_tables(false, list);
    if (ret != OB_SUCCESS)
    {
      TBSYS_LOG(WARN, "drop root table table failed:table_id[%lu], ret[%d]", table_id, ret);
    }
  }
  return ret;
}

bool ObRootDDLOperator::delete_schema_table(const common::ObString & table_name, uint64_t & table_id)
{
  bool succ = false;
  tbsys::CThreadGuard lock(&mutex_lock_);
  int ret = schema_client_->get_table_id(table_name, table_id);
  if (ret != OB_SUCCESS)
  {
    TBSYS_LOG(WARN, "get table id failed stop drop:table_name[%.*s], ret[%d]",
        table_name.length(), table_name.ptr(), ret);
  }
  else
  {
    ret = schema_client_->drop_table(table_name);
    if (ret != OB_SUCCESS)
    {
      TBSYS_LOG(WARN, "delete schema table failed:table_name[%.*s], ret[%d]",
          table_name.length(), table_name.ptr(), ret);
    }
    else
    {
      succ = true;
    }
    // double check table schema content
    uint64_t temp_table_id = 0;
    ret = schema_client_->get_table_id(table_name, temp_table_id);
    if (ret != OB_SUCCESS)
    {
      if (OB_ENTRY_NOT_EXIST == ret)
      {
        succ = true;
      }
    }
    else
    {
      TBSYS_LOG(WARN, "check get table id succ after delete:table_name[%.*s], table_id[%lu]",
          table_name.length(), table_name.ptr(), temp_table_id);
      succ = false;
    }
  }
  return succ;
}

int ObRootDDLOperator::alter_table(AlterTableSchema & table_schema)
{
  int ret = OB_SUCCESS;
  if (!check_inner_stat())
  {
    ret = OB_ERROR;
    TBSYS_LOG(WARN, "check inner stat failed");
  }
  else
  {
    TableSchema old_schema;
    int64_t len = strlen(table_schema.table_name_);
    ObString table_name(static_cast<int32_t>(len), static_cast<int32_t>(len), table_schema.table_name_);
    tbsys::CThreadGuard lock(&mutex_lock_);
    // get table schema from inner table
    ret = schema_client_->get_table_schema(table_name, old_schema);
    if (ret != OB_SUCCESS)
    {
      TBSYS_LOG(WARN, "get table schema failed:table_name[%s], ret[%d]",
          table_schema.table_name_, ret);
    }
    else if (old_schema.table_id_ < OB_ALL_JOIN_INFO_TID)
    {
      TBSYS_LOG(WARN, "check table id failed:table_id[%lu]", old_schema.table_id_);
      ret = OB_INVALID_ARGUMENT;
    }
    else
    {
      // modify table schema info
      table_schema.table_id_ = old_schema.table_id_;
      ret = alter_table_schema(old_schema, table_schema);
      if (ret != OB_SUCCESS)
      {
        TBSYS_LOG(WARN, "alter table schema failed:table_name[%s], table_id[%lu], ret[%d]",
            table_schema.table_name_, table_schema.table_id_, ret);
      }
    }
    // alter inner table
    if (OB_SUCCESS == ret)
    {
      ret = schema_client_->alter_table(table_schema, old_schema.schema_version_);
      if (ret != OB_SUCCESS)
      {
        TBSYS_LOG(WARN, "alter table failed:table_name[%s], table_id[%lu], ret[%d]",
            table_schema.table_name_, table_schema.table_id_, ret);
      }
      else
      {
        TBSYS_LOG(INFO, "alter table modify inner table succ:table_name[%s], table_id[%lu]",
            table_schema.table_name_, table_schema.table_id_);
      }
    }
  }
  return ret;
}

int ObRootDDLOperator::alter_table_schema(const TableSchema & old_schema, AlterTableSchema & alter_schema)
{
  int ret = OB_SUCCESS;
  const char * column_name = NULL;
  int64_t column_count = alter_schema.get_column_count();
  AlterColumn column_schema;
  uint64_t max_column_id = old_schema.max_used_column_id_;
  // check old table schema max column id
  if (max_column_id < OB_APP_MIN_COLUMN_ID)
  {
    ret = OB_INVALID_ARGUMENT;
    TBSYS_LOG(WARN, "check old table schema max column id failed:max[%lu], min[%lu]",
        max_column_id, OB_APP_MIN_COLUMN_ID);
  }
  else
  {
    // check if not exist allocate the column id
    for (int64_t i = 0; (OB_SUCCESS == ret) && (i < column_count); ++i)
    {
      column_name = alter_schema.columns_.at(i).column_.column_name_;
      // check and modify column id
      ret = check_alter_column(old_schema, alter_schema.columns_.at(i));
      if (ret != OB_SUCCESS)
      {
        TBSYS_LOG(WARN, "check column is valid failed:table[%s], column[%s], ret[%d]",
            alter_schema.table_name_, column_name, ret);
        break;
      }
      else
      {
        ret = set_column_info(old_schema, column_name, max_column_id, alter_schema.columns_.at(i));
        if (ret != OB_SUCCESS)
        {
          TBSYS_LOG(WARN, "set column info failed:table[%s], column[%s], ret[%d]",
              alter_schema.table_name_, column_name, ret);
          break;
        }
      }
    }
  }
  return ret;
}

int ObRootDDLOperator::check_alter_column(const TableSchema & old_schema,
    AlterTableSchema::AlterColumnSchema & alter_column)
{
  int ret = OB_SUCCESS;
  ObObjType data_type = alter_column.column_.data_type_;
  // create or modify datetime column not allow
  if ((data_type == ObCreateTimeType) || (data_type == ObModifyTimeType))
  {
    ret = OB_OP_NOT_ALLOW;
    TBSYS_LOG(WARN, "column type is create or modify time not allowed:type[%d]", data_type);
  }
  // rowkey column not allow
  else if (alter_column.column_.rowkey_id_ > 0)
  {
    ret = OB_OP_NOT_ALLOW;
    TBSYS_LOG(WARN, "alter primary rowkey column not allowed:table[%s], column[%s], rowkey_id[%ld]",
        old_schema.table_name_, alter_column.column_.column_name_, alter_column.column_.rowkey_id_);
  }
  return ret;
}

// set old column id or allocate new column id
int ObRootDDLOperator::set_column_info(const TableSchema & old_schema, const char * column_name,
    uint64_t & max_column_id, AlterTableSchema::AlterColumnSchema & alter_column)
{
  int ret = OB_SUCCESS;
  const ColumnSchema * old_column = old_schema.get_column_schema(column_name);
  // column already exist
  if (old_column != NULL)
  {
    switch (alter_column.type_)
    {
    case AlterTableSchema::ADD_COLUMN:
      {
        ret = OB_ENTRY_EXIST;
        TBSYS_LOG(WARN, "column already exist can not add again:column[%s], ret[%d]",
            column_name, ret);
        break;
      }
    case AlterTableSchema::DEL_COLUMN:
      {
        // TODO fill other property and check not join column
        alter_column.column_.column_id_ = old_column->column_id_;
        break;
      }
    default:
      {
        TBSYS_LOG(WARN, "modify exist column not supported right now");
        ret = OB_INVALID_ARGUMENT;
        break;
      }
    }
  }
  // column not exist
  else
  {
    if (alter_column.type_ != AlterTableSchema::ADD_COLUMN)
    {
      ret = OB_ENTRY_NOT_EXIST;
      TBSYS_LOG(WARN, "column does not exist can not drop:column[%s], ret[%d]",
          column_name, ret);
    }
    else
    {
      // allocate new column id
      alter_column.column_.column_id_ = ++max_column_id;
    }
  }
  return ret;
}
int ObRootDDLOperator::modify_table_id(common::TableSchema &table_schema, const int64_t new_table_id)
{
  int ret = OB_SUCCESS;
  if (!check_inner_stat())
  {
    ret = OB_ERROR;
    TBSYS_LOG(WARN, "check inner stat failed");
  }
  if (OB_SUCCESS == ret)
  {
    tbsys::CThreadGuard lock(&mutex_lock_);
    ObString table_name;
    int ret = schema_client_->get_table_name(new_table_id, table_name);
    if (ret != OB_ENTRY_NOT_EXIST)
    {
      TBSYS_LOG(WARN, "table id is already been used. table_id=%ld, table_name=%s",
          new_table_id, to_cstring(table_name));
      ret = OB_ERROR;
    }
    else
    {
      ret = schema_client_->modify_table_id(table_schema, new_table_id);
      if (OB_SUCCESS != ret)
      {
        TBSYS_LOG(WARN, "fail to modify table id. new_table_id=%ld", new_table_id);
      }
    }
  }
  return ret;
}

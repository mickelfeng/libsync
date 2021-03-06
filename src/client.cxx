/*
  The filesync client wrapper

  Copyright (C) 2012 William A. Kennington III

  This file is part of Libsync.

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <iostream>
#include <fstream>
#include <functional>
#include <chrono>
#include <sys/types.h>
#include <sys/stat.h>
#ifdef WIN32
#  include <sys/utime.h>
#  include <time.h>
#else
#  include <utime.h>
#endif

#include "client.hxx"
#include "log.hxx"
#include "util.hxx"

Client::Client(const Config & conf)
  : done(false), conf(conf), conn(NULL), crypt(NULL), meta(NULL),
    file_thread(NULL), pull_thread(NULL), watch_thread(NULL)
{
  Metadata *remote = NULL;

  try
    {
      // Load the local metadata from the sync directory
      if (!conf.exists("sync_dir"))
        throw "Client must specify synchronization directory";
      sync_dir = conf.get_str("sync_dir");
      meta = new Metadata(sync_dir);

      // Attempt to create the connection type specified in the config
      if (!conf.exists("conn") || conf.get_str("conn") == "sock")
        {
          if (!conf.exists("conn_host") || !conf.exists("conn_port") ||
              !conf.exists("conn_user") || !conf.exists("conn_pass"))
            throw "Socket Connector Missing Parameters";
          if (conf.exists("key"))
            conn = new SockConnector(conf.get_str("conn_host"),
                                     conf.get_int("conn_port"),
                                     conf.get_str("conn_user"),
                                     conf.get_str("conn_pass"),
                                     conf.get_str("key"));
          else
            conn = new SockConnector(conf.get_str("conn_host"),
                                     conf.get_int("conn_port"),
                                     conf.get_str("conn_user"),
                                     conf.get_str("conn_pass"));
        }
      else
        throw "Unrecognized connector type - " + conf.get_str("conn");

      // Get the remote metadata and perform a merge with local metadata
      global_log.message("Getting the remote metadata", Log::NOTICE);
      remote = conn->get_metadata();
      merge_metadata(*remote);
    }
  catch(const char * e)
    {
      global_log.message(e, 1);
      delete meta;
      delete conn;
      delete remote;
      throw e;
    }
  catch(const std::string & e)
    {
      global_log.message(e, 1);
      delete meta;
      delete conn;
      delete remote;
      throw e;
    }
  delete remote;

  global_log.message("Client successfully started!", Log::NOTICE);
}

Client::~Client()
{
  // Forcibly close the connections
  wd.close();
  conn->close();

  done = true;
  message_cond.notify_all();

  // Wait for the threads to finish
  file_thread->join();
  pull_thread->join();
  watch_thread->join();

  // Remove allocated data
  delete file_thread;
  delete pull_thread;
  delete watch_thread;
  delete meta;
  delete conn;
}

void Client::start()
{
  global_log.message("Spawning Client Threads", Log::NOTICE);
  file_thread = new std::thread(std::bind(&Client::file_master, this));
  pull_thread = new std::thread(std::bind(&Client::pull_master, this));
  wd.add_watch(sync_dir, true);
  watch_thread = new std::thread(std::bind(&Client::watch_master, this));
}

void Client::merge_metadata(const Metadata & remote)
{
  Msg msg;

  // Merge all of the local data into the remote data and push messages
  for (auto it = meta->begin(), end = meta->end(); it != end; it++)
    {
      Metadata::Data rem = remote.get_file(it->first);

      // Is the file at least better than the file on the server
      if (it->second.modified <= rem.modified)
        continue;

      // Parse the data into a message
      msg.filename = it->first;
      msg.remote = false;
      msg.file_data = it->second;

      global_log.message(std::string("Local Push: ") + it->first,
                         Log::DEBUG);

      // Push the message onto the stack
      message_lock.lock();
      messages.push(msg);
      message_lock.unlock();
      message_cond.notify_all();
    }

  // Merge the remote data into the local data and pull messages
  for (auto it = remote.begin(), end = remote.end(); it != end; it++)
    {
      Metadata::Data local =  meta->get_file(it->first);

      // Is the file at least better than the local file
      if (it->second.modified <= local.modified)
        continue;

      // Parse the data into a message
      msg.filename = it->first;
      msg.remote = true;
      msg.file_data = it->second;

      global_log.message(std::string("Remote Push: ") + it->first,
                         Log::DEBUG);

      // Push the message onto the stack
      message_lock.lock();
      messages.push(msg);
      message_lock.unlock();
      message_cond.notify_all();
    }
}

void Client::file_master()
{
  Msg msg;

  message_lock.lock();
  while(!done)
    {
      // Get the next message
      if (messages.empty())
        {
          message_cond.wait(message_lock);
          continue;
        }
      msg = messages.front();
      messages.pop();
      message_lock.unlock();

      // Is this event old?
      Metadata::Data data = meta->get_file(msg.filename);
      if (msg.file_data.deleted == data.deleted &&
          msg.file_data.modified < data.modified)
        {
          global_log.message(std::string("Skipped Event: ") +
                             msg.filename, Log::NOTICE);
          message_lock.lock();
          continue;
        }

      // Parse the event
      std::string full_name = sync_dir + msg.filename;
      if (msg.remote)
        {
          // The file is remotely changed so disable local events on it
          wd.disregard(full_name);

          if (msg.file_data.deleted)
            {
              global_log.message(std::string("Remote Delete: ") + full_name,
                                 Log::NOTICE);
              File::recursive_remove(full_name);
            }
          else
            {
              global_log.message(std::string("Remote Modify: ") + full_name,
                                 Log::NOTICE);
              std::ofstream out(full_name, std::ios::out | std::ios::binary);
              conn->get_file(msg.filename, msg.file_data.modified, out);
              out.close();

              struct utimbuf tim;
              tim.actime = time(NULL);
              tim.modtime = msg.file_data.modified;
              utime(full_name.c_str(), &tim);
            }

          // Allow events again
          wd.regard(full_name);
        }
      else
        if (msg.file_data.deleted)
          {
            global_log.message(std::string("Local Delete: ") + full_name,
                               Log::NOTICE);
            conn->delete_file(msg.filename, msg.file_data.modified);
          }
        else
          {
            global_log.message(std::string("Local Modify: ") + full_name,
                               Log::NOTICE);
            std::ifstream in(full_name, std::ios::in | std::ios::binary);
            struct stat stats;
            stat(full_name.c_str(), &stats);
            try
              {
                conn->push_file(msg.filename, stats.st_mtime,
                                in, stats.st_size);
              }
            catch (const char * e)
              {
                global_log.message(e, Log::WARNING);
              }
            catch (const std::string & e)
              {
                global_log.message(e, Log::WARNING);
              }
            in.close();
          }
      global_log.message(std::string("Finished Processing: ") + full_name,
                         Log::NOTICE);

      // Relock for the next message
      message_lock.lock();
   }
  message_lock.unlock();
}

void Client::pull_master()
{
  std::pair<std::string, Metadata::Data> data;
  try
    {
      while(true)
        {
          // Wait for the message
          data = conn->wait();

          // Parse the message
          Msg msg;
          msg.remote = true;
          msg.filename = data.first;
          msg.file_data = data.second;

          // Push the message onto the stack
          message_lock.lock();
          messages.push(msg);
          message_lock.unlock();
          message_cond.notify_all();
        }
    }
  catch(const char * e)
    {}
  catch(const std::string & e)
    {}
}

void Client::watch_master()
{
  Msg msg;

  try
    {
      global_log.message("Started Watchdog", Log::NOTICE);
      while(true)
        {
          // Wait for the watchdog to return events
          Watchdog::Data data = wd.wait();

          // Parse the event into a file event
          msg.remote = false;
          msg.filename = data.filename.substr(sync_dir.length());
          msg.file_data.modified = data.modified;
          msg.file_data.deleted = data.status == Watchdog::FileStatus::deleted;

          // Push the message onto the stack
          message_lock.lock();
          messages.push(msg);
          message_lock.unlock();
          message_cond.notify_all();
        }
    }
  catch(const char * e)
    {}
  catch(const std::string & e)
    {}
}

#include "pqxx-source.hxx"

#include <cerrno>
#include <stdexcept>

#include <libpq-fe.h>

#include "pqxx/blob"
#include "pqxx/except"
#include "pqxx/internal/concat.hxx"
#include "pqxx/internal/gates/connection-largeobject.hxx"


namespace
{
constexpr int INV_WRITE{0x00020000}, INV_READ{0x00040000};
} // namespace


pqxx::internal::pq::PGconn *
pqxx::blob::raw_conn(pqxx::connection *conn) noexcept
{
  pqxx::internal::gate::connection_largeobject gate{*conn};
  return gate.raw_connection();
}


pqxx::internal::pq::PGconn *
pqxx::blob::raw_conn(pqxx::dbtransaction const &tx) noexcept
{
  return raw_conn(&tx.conn());
}


std::string pqxx::blob::errmsg(connection const *conn)
{
  pqxx::internal::gate::const_connection_largeobject gate{*conn};
  return gate.error_message();
}


pqxx::blob pqxx::blob::open_internal(dbtransaction &tx, oid id, int mode)
{
  auto &conn{tx.conn()};
  int fd{lo_open(raw_conn(&conn), id, mode)};
  if (fd == -1)
    throw pqxx::failure{internal::concat(
      "Could not open binary large object ", id, ": ", errmsg(&conn))};
  return pqxx::blob{conn, fd};
}


pqxx::oid pqxx::blob::create(dbtransaction &tx, oid id)
{
  oid actual_id{lo_create(raw_conn(tx), id)};
  if (actual_id == 0)
    throw failure{internal::concat(
      "Could not create binary large object: ", errmsg(&tx.conn()))};
  return actual_id;
}


void pqxx::blob::remove(dbtransaction &tx, oid id)
{
  if (id == 0)
    throw usage_error{"Trying to delete binary large object without an ID."};
  if (lo_unlink(raw_conn(tx), id) == -1)
    throw failure{internal::concat(
      "Could not delete large object ", id, ": ", errmsg(&tx.conn()))};
}


pqxx::blob pqxx::blob::open_r(dbtransaction &tx, oid id)
{
  return open_internal(tx, id, INV_READ);
}


pqxx::blob pqxx::blob::open_w(dbtransaction &tx, oid id)
{
  return open_internal(tx, id, INV_WRITE);
}


pqxx::blob pqxx::blob::open_rw(dbtransaction &tx, oid id)
{
  return open_internal(tx, id, INV_READ | INV_WRITE);
}


pqxx::blob::blob(blob &&other) : m_conn{other.m_conn}, m_fd{other.m_fd}
{
  other.m_conn = nullptr;
  other.m_fd = -1;
}


pqxx::blob &pqxx::blob::operator=(blob &&other)
{
  if (m_fd != -1)
    lo_close(raw_conn(m_conn), m_fd);
  m_conn = other.m_conn;
  m_fd = other.m_fd;
  other.m_conn = nullptr;
  other.m_fd = -1;
  return *this;
}


pqxx::blob::~blob()
{
  try
  {
    close();
  }
  catch (std::exception const &)
  {}
}


void pqxx::blob::close()
{
  if (m_fd != -1)
    lo_close(raw_conn(m_conn), m_fd);
}


void pqxx::blob::read(std::basic_string<std::byte> &buf, std::size_t size)
{
  if (m_conn == nullptr)
    throw usage_error{"Attempt to read from a closed binary large object."};
  buf.resize(size);
  auto data{reinterpret_cast<char *>(buf.data())};
  int received{lo_read(raw_conn(m_conn), m_fd, data, size)};
  if (received < 0)
    throw failure{
      internal::concat("Could not read from binary large object: ", errmsg())};
  buf.resize(static_cast<std::size_t>(received));
}


void pqxx::blob::write(std::basic_string_view<std::byte> buf)
{
  if (m_conn == nullptr)
    throw usage_error{"Attempt to write to a closed binary large object."};
  auto ptr{reinterpret_cast<char const *>(buf.data())};
  int written{lo_write(raw_conn(m_conn), m_fd, ptr, buf.size())};
  if (written < 0)
    throw failure{
      internal::concat("Write to binary large object failed: ", errmsg())};
}


void pqxx::blob::resize(std::int64_t size)
{
  if (m_conn == nullptr)
    throw usage_error{"Attempt to resize a closed binary large object."};
  if (lo_truncate64(raw_conn(m_conn), m_fd, size) < 0)
    throw failure{
      internal::concat("Binary large object truncation failed: ", errmsg())};
}


std::int64_t pqxx::blob::tell() const
{
  if (m_conn == nullptr)
    throw usage_error{"Attempt to tell() a closed binary large object."};
  std::int64_t offset{lo_tell64(raw_conn(m_conn), m_fd)};
  if (offset < 0)
    throw failure{internal::concat(
      "Error reading binary large object position: ", errmsg())};
  return offset;
}


std::int64_t pqxx::blob::seek(std::int64_t offset, int whence)
{
  if (m_conn == nullptr)
    throw usage_error{"Attempt to seek() a closed binary large object."};
  std::int64_t seek_result{lo_lseek64(raw_conn(m_conn), m_fd, offset, whence)};
  if (seek_result < 0)
    throw failure{internal::concat(
      "Error during seek on binary large object: ", errmsg())};
  return seek_result;
}


std::int64_t pqxx::blob::seek_abs(std::int64_t offset)
{
  return this->seek(offset, SEEK_SET);
}


std::int64_t pqxx::blob::seek_rel(std::int64_t offset)
{
  return this->seek(offset, SEEK_CUR);
}


std::int64_t pqxx::blob::seek_end(std::int64_t offset)
{
  return this->seek(offset, SEEK_END);
}


pqxx::oid pqxx::blob::from_buf(
  dbtransaction &tx, std::basic_string_view<std::byte> data, oid id)
{
  oid actual_id{create(tx, id)};
  try
  {
    open_w(tx, actual_id).write(data);
  }
  catch (std::exception const &)
  {
    try
    {
      remove(tx, id);
    }
    catch (std::exception const &e)
    {}
    throw;
  }
  return actual_id;
}


void pqxx::blob::to_buf(
  dbtransaction &tx, oid id, std::basic_string<std::byte> &buf,
  std::int64_t max_size)
{
  open_r(tx, id).read(buf, max_size);
}


pqxx::oid pqxx::blob::from_file(dbtransaction &tx, char const path[])
{
  auto id{lo_import(raw_conn(tx), path)};
  if (id == 0)
    throw failure{internal::concat(
      "Could not import '", path, "' as a binary large object: ", errmsg(tx))};
  return id;
}


pqxx::oid pqxx::blob::from_file(dbtransaction &tx, char const path[], oid id)
{
  auto actual_id{lo_import_with_oid(raw_conn(tx), path, id)};
  if (actual_id == 0)
    throw failure{internal::concat(
      "Could not import '", path, "' as binary large object ", id, ": ",
      errmsg(tx))};
  return actual_id;
}


void pqxx::blob::to_file(dbtransaction &tx, oid id, char const path[])
{
  if (lo_export(raw_conn(tx), id, path) < 0)
    throw failure{internal::concat(
      "Could not export binary large object ", id, " to file '", path,
      "': ", errmsg(tx))};
}
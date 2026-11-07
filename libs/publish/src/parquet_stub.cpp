// Without Apache Arrow there is no Parquet writer: a build writes everything
// but data/*.parquet and says so (PYCHRON_PUBLISH, libs/publish/CMakeLists.txt).

#include "nobion/publish/parquet.hpp"

#ifndef PYCHRON_PUBLISH_ARROW_ENABLED

namespace nobion::publish {

std::unique_ptr<IParquetWriter> make_parquet_writer() { return nullptr; }
std::string_view parquet_writer_name() { return {}; }

}  // namespace nobion::publish

#endif  // !PYCHRON_PUBLISH_ARROW_ENABLED

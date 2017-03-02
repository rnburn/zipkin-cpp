#include "HttpCollector.h"

#include <glog/logging.h>

#include <thrift/transport/TBufferTransports.h>
#include <thrift/protocol/TBinaryProtocol.h>

#define RAPIDJSON_HAS_STDSTRING 1
#include <rapidjson/stringbuffer.h>
#include <rapidjson/prettywriter.h>

namespace zipkin
{

HttpCollector *HttpConf::create(void) const
{
    return new HttpCollector(*this);
}

HttpCollector::HttpCollector(const HttpConf &conf)
    : m_conf(conf), m_spans(conf.backlog), m_queued_spans(0),
      m_terminated(false), m_worker(HttpCollector::run, this)
{
}

HttpCollector::~HttpCollector()
{
    flush(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::microseconds(500)));
}

bool HttpCollector::drop_front_span()
{
    CachedSpan *span = nullptr;

    if (m_spans.pop(span) && span)
    {
        LOG(WARNING) << "Drop Span `" << std::hex << span->id() << " exceed backlog";

        span->release();

        return true;
    }

    return false;
}

void HttpCollector::submit(Span *span)
{
    Span *old = nullptr;

    while (m_queued_spans >= m_conf.backlog)
    {
        if (drop_front_span())
            m_queued_spans--;
    }

    if (m_spans.push(span))
        m_queued_spans++;

    if (m_queued_spans >= m_conf.batch_size)
        flush(std::chrono::milliseconds(0));
}

bool HttpCollector::flush(std::chrono::milliseconds timeout_ms)
{
    VLOG(2) << "flush pendding " << m_queued_spans << " spans";

    if (m_spans.empty())
        return true;

    m_flush.notify_one();

    std::unique_lock<std::mutex> lock(m_flushing);

    return m_sent.wait_for(lock, timeout_ms, [this] { return m_spans.empty(); });
}

void HttpCollector::try_send_spans(void)
{
    std::unique_lock<std::mutex> lock(m_sending);

    if (m_flush.wait_for(lock, m_conf.batch_interval, [this] { return !m_spans.empty(); }))
    {
        send_spans();
    }
}

void HttpCollector::send_spans(void)
{
    VLOG(2) << "sending " << m_queued_spans << " spans";

    CachedSpan *span = nullptr;
    std::vector<Span *> spans;

    m_queued_spans -= m_spans.consume_all([&spans](Span *span) {
        spans.push_back(span);
    });

    boost::shared_ptr<apache::thrift::transport::TMemoryBuffer> buf(new apache::thrift::transport::TMemoryBuffer());

    m_conf.message_codec->encode(buf, spans);

    for (auto span : spans)
    {
        span->release();
    }

    m_sent.notify_all();
}

void HttpCollector::run(HttpCollector *collector)
{
    VLOG(1) << "HTTP collector started";

    do
    {
        collector->try_send_spans();

        std::this_thread::yield();
    } while (!collector->m_terminated);

    VLOG(1) << "HTTP collector stopped";
}

} // namespace zipkin
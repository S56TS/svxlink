// Included after production QsoFrn.cpp by test_upstream_merge.py.
// ModuleFrn is only a config provider here; sockets/codecs/parsers are real.
#include <sys/socket.h>
#include <unistd.h>

struct FrnCapture : Async::AudioSink
{
  std::vector<float> samples;
  int writeSamples(const float* data, int count) override
  {
    samples.insert(samples.end(), data, data + count);
    return count;
  }
  void flushSamples() override { sourceAllSamplesFlushed(); }
};

struct FrnFixture
{
  ModuleFrn module;
  std::unique_ptr<QsoFrn> qso;
  int peer = -1;
  FrnFixture()
  {
    for (const char* key : {"SERVER", "PORT", "SERVER_BACKUP", "PORT_BACKUP",
         "EMAIL_ADDRESS", "DYN_PASSWORD", "CALLSIGN_AND_USER", "CLIENT_TYPE",
         "BAND_AND_CHANNEL", "DESCRIPTION", "COUNTRY", "CITY_CITY_PART", "NET", "VERSION"})
      module.cfg().setValue("Frn", key, "test");
    module.cfg().setValue("Frn", "SERVER", "127.0.0.1");
    module.cfg().setValue("Frn", "SERVER_BACKUP", "127.0.0.1");
    module.cfg().setValue("Frn", "PORT", "9");
    module.cfg().setValue("Frn", "PORT_BACKUP", "9");
    qso.reset(new QsoFrn(&module));
    assert(qso->initOk());
    int sockets[2]; assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    qso->tcp_client->setSocket(sockets[0]); peer = sockets[1];
    qso->state = QsoFrn::STATE_IDLE;
  }
  ~FrnFixture() { qso.reset(); if (peer >= 0) close(peer); }
  int feed(std::string bytes)
  {
    return qso->onDataReceived(qso->tcp_client, &bytes[0], bytes.size());
  }
  std::string wire()
  {
    // Drive the real writable callback; Async queues writes until its event
    // loop signals the socket. No mock encoding or TCP write implementation.
    auto* client = qso->tcp_client;
    for (int n = 0; client->isConnected() && !client->m_write_buf.empty(); ++n)
    {
      assert(n < 100);
      client->onWriteSpaceAvailable(&client->m_wr_watch);
    }
    std::string result; char buf[8192]; ssize_t n;
    while ((n = recv(peer, buf, sizeof(buf), MSG_DONTWAIT)) > 0) result.append(buf, n);
    return result;
  }
};

int main()
{
  Async::CppApplication app;
  {
    FrnFixture f; auto& q = *f.qso;
    std::vector<std::string> text;
    q.textMessageReceived.connect([&](const std::string& from, const std::string& msg,
                                    const std::string& scope) { text = {from, msg, scope}; });
    std::string payload(1, QsoFrn::DT_TEXT_MESSAGE);
    payload += "3\r\n123\nRunCmd: get stats\r\nP\n";
    assert(f.feed(payload) == static_cast<int>(payload.size()));
    assert((text == std::vector<std::string>{"123", "RunCmd: get stats", "P"}));
    assert(q.state == QsoFrn::STATE_IDLE && q.cur_item_list.empty());
    text.clear();
    payload.assign(1, QsoFrn::DT_TEXT_MESSAGE);
    payload += "1\n<ID>456</ID><MS>RunCmd: help</MS>\r\n";
    assert(f.feed(payload) == static_cast<int>(payload.size()));
    assert((text == std::vector<std::string>{"456", "RunCmd: help", "P"}));
    for (const auto& count : {std::string("-1\n"), std::string("bad\r\n")})
    {
      payload.assign(1, QsoFrn::DT_NET_NAMES); payload += count;
      assert(f.feed(payload) == static_cast<int>(payload.size()));
      assert(q.state == QsoFrn::STATE_IDLE && q.lines_to_read == -1);
    }
    payload.assign(1, QsoFrn::DT_NET_NAMES); payload += "999999999999999999999999\n";
    assert(f.feed(payload) == static_cast<int>(payload.size()));
    assert(q.lines_to_read == 10000 && q.cur_item_list.empty());
    q.state = QsoFrn::STATE_IDLE;
    q.cur_item_list.push_back("stale"); q.lines_to_read = 77;
    assert(f.feed(std::string(1, QsoFrn::DT_TEXT_MESSAGE)) == 1);
    assert(q.lines_to_read == -1 && q.cur_item_list.empty());
  }
  {
    FrnFixture f; auto& q = *f.qso;
    q.state = QsoFrn::STATE_LOGGING_IN_1;
    assert(f.feed("2014003") == 0);
    std::string login = "2014003\r\n<AL>OK</AL>\n";
    assert(f.feed(login) == static_cast<int>(login.size()));
    assert(q.state == QsoFrn::STATE_IDLE && f.wire() == "RX0\r\n");
    q.connect_retry_cnt = 8; q.reconnect_timeout_ms = 9999;
    q.state = QsoFrn::STATE_LOGGING_IN_2;
    std::string reply = "<AL>OK</AL>\r\n";
    assert(f.feed(reply) == static_cast<int>(reply.size()));
    assert(q.connect_retry_cnt == 0 && q.reconnect_timeout_ms == q.RECONNECT_TIMEOUT_TIME);
    q.state = QsoFrn::STATE_LOGGING_IN_1;
    assert(f.feed("invalid login\n") == 14);
    assert(q.state == QsoFrn::STATE_DISCONNECTED && q.reconnect_timer->isEnabled());
  }
  {
    FrnFixture f; auto& q = *f.qso;
    std::vector<float> audio(q.BUFFER_SIZE + 100, 0.2f);
    assert(q.writeSamples(audio.data(), audio.size()) == static_cast<int>(audio.size()));
    assert(q.pending_tx_frames.size() == 1 && q.send_buffer_cnt == 100);
    q.flushSamples();
    assert(q.tx_end_pending && f.wire() == "TX0\r\n");
    std::string grant(1, QsoFrn::DT_DO_TX); grant.push_back(0);
    assert(f.feed(grant) == 1); // Split client index stays unconsumed.
    assert(q.state == QsoFrn::STATE_TX_AUDIO_APPROVED && q.tx_voice_packets == 0);
    assert(f.feed(std::string("\0\1", 2)) == 2);
    assert(q.pending_tx_frames.empty() && !q.tx_end_pending && q.send_buffer_cnt == 0);
    assert(q.tx_voice_packets == 2 && !q.tx_wait_timer->isEnabled());
    std::string wire = f.wire();
    const size_t packet = 5 + q.FRN_AUDIO_PACKET_SIZE;
    assert(wire.size() == packet * 2 + 5);
    assert(wire.substr(0, 5) == "TX1\r\n" && wire.substr(packet, 5) == "TX1\r\n");
    assert(wire.substr(packet * 2) == "RX0\r\n");
    // WAV49 reference codec verifies framing and independent TX/RX state.
    assert(q.gsmh_tx != q.gsmh_rx);
    gsm ref = 0; assert(initGsmWav49(ref, "test"));
    std::vector<short> pcm(q.BUFFER_SIZE, static_cast<short>(32767.0f * 0.2f));
    std::vector<unsigned char> encoded(q.FRN_AUDIO_PACKET_SIZE);
    for (int n = 0; n < q.FRAME_COUNT; ++n)
    {
      gsm_encode(ref, pcm.data() + n * q.PCM_FRAME_SIZE, encoded.data() + n * q.GSM_FRAME_SIZE);
      gsm_encode(ref, pcm.data() + n * q.PCM_FRAME_SIZE + q.PCM_FRAME_SIZE / 2,
                 encoded.data() + n * q.GSM_FRAME_SIZE + 32);
    }
    assert(std::memcmp(wire.data() + 5, encoded.data(), encoded.size()) == 0);
    gsm_destroy(ref); ref = 0;
    // Receive the same WAV49 bytes through the production RX path and compare
    // against a fresh decoder. Interleaved RX must not disturb TX encoding.
    FrnCapture capture;
    assert(q.registerSink(&capture, false));
    q.state = QsoFrn::STATE_RX_AUDIO;
    std::string received("\0\1", 2);
    received.append(reinterpret_cast<const char*>(encoded.data()), encoded.size());
    assert(f.feed(received) == static_cast<int>(received.size()));
    assert(capture.samples.size() == q.BUFFER_SIZE);
    assert(initGsmWav49(ref, "RX reference"));
    std::vector<short> decoded(q.BUFFER_SIZE);
    for (int n = 0; n < q.FRAME_COUNT; ++n)
    {
      assert(gsm_decode(ref, encoded.data() + n * q.GSM_FRAME_SIZE,
                        decoded.data() + n * q.PCM_FRAME_SIZE) != -1);
      assert(gsm_decode(ref, encoded.data() + n * q.GSM_FRAME_SIZE + 33,
                        decoded.data() + n * q.PCM_FRAME_SIZE + q.PCM_FRAME_SIZE / 2) != -1);
    }
    gsm_destroy(ref);
    for (int n = 0; n < q.BUFFER_SIZE; ++n)
      assert(capture.samples[n] == decoded[n] / 32768.0f);
    q.unregisterSink();
    f.wire();
    q.state = QsoFrn::STATE_TX_AUDIO_WAITING;
    for (int n = 0; n < q.TX_GRANT_BUFFER_FRAMES + 5; ++n)
      assert(q.writeSamples(audio.data(), q.BUFFER_SIZE) == q.BUFFER_SIZE);
    assert(q.pending_tx_frames.size() == q.TX_GRANT_BUFFER_FRAMES);
    q.onTxWaitTimeout(q.tx_wait_timer);
    assert(q.state == QsoFrn::STATE_DISCONNECTED && q.pending_tx_frames.empty());
    assert(q.reconnect_timer->isEnabled());
  }
  {
    FrnFixture f; auto& q = *f.qso;
    close(f.peer); f.peer = -1;
    assert(q.sendRequest(QsoFrn::RQ_P));
    f.wire();
    assert(q.state == QsoFrn::STATE_DISCONNECTED && q.reconnect_timer->isEnabled());
  }
  {
    FrnFixture f; auto& q = *f.qso;
    q.state = QsoFrn::STATE_RX_LIST;
    assert(q.sendTextMessage("123", "CmdReply: a<&>"));
    assert(f.wire().empty() && q.tm_out_queue.size() == 1);
    q.state = QsoFrn::STATE_IDLE;
    q.tmOutSendNext(q.tm_out_timer);
    assert(f.wire() == "TM:<ID>123</ID><MS>CmdReply: a&lt;&amp;&gt;</MS>\r\n");
    assert(q.tm_out_queue.empty());
    q.state = QsoFrn::STATE_LOGGING_IN_2;
    std::string blocked = "<AL>BLOCK</AL>\r\n";
    assert(f.feed(blocked) == static_cast<int>(blocked.size()));
    assert(q.state == QsoFrn::STATE_DISCONNECTED && q.reconnect_timer->isEnabled());
  }
  {
    FrnFixture f; auto& q = *f.qso;
    q.state = QsoFrn::STATE_RX_AUDIO;
    q.pending_tx_frames.push_back({});
    q.onConnectTimeout(q.con_timeout_timer);
    assert(q.state == QsoFrn::STATE_CONNECTING && q.pending_tx_frames.empty());
    assert(q.connect_retry_cnt == 1 && q.con_timeout_timer->isEnabled());
  }
  std::cout << "PASS FRN: bounded/mixed-newline lists, RunCmd text delivery, login/recovery, "
               "split grants, buffered flush, WAV49 bytes, separate codecs, queue bounds/watchdogs\n";
}

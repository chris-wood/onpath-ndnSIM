/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/**
 * Copyright (c) 2011-2015  Regents of the University of California.
 *
 * This file is part of ndnSIM. See AUTHORS for complete list of ndnSIM authors and
 * contributors.
 *
 * ndnSIM is free software: you can redistribute it and/or modify it under the terms
 * of the GNU General Public License as published by the Free Software Foundation,
 * either version 3 of the License, or (at your option) any later version.
 *
 * ndnSIM is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 * PURPOSE.  See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * ndnSIM, e.g., in COPYING.md file.  If not, see <http://www.gnu.org/licenses/>.
 **/

#include "ndn-verifying-consumer.hpp"

#include "ns3/ptr.h"
#include "ns3/log.h"
#include "ns3/simulator.h"
#include "ns3/packet.h"
#include "ns3/callback.h"
#include "ns3/string.h"
#include "ns3/boolean.h"
#include "ns3/uinteger.h"
#include "ns3/integer.h"
#include "ns3/double.h"

#include <ndn-cxx/security/validator.hpp>

#include "utils/ndn-ns3-packet-tag.hpp"
#include "model/ndn-app-face.hpp"
#include "utils/ndn-rtt-mean-deviation.hpp"

#include <boost/lexical_cast.hpp>
#include <boost/ref.hpp>

NS_LOG_COMPONENT_DEFINE("ndn.VerifyingConsumer");

namespace ns3 {
namespace ndn {

NS_OBJECT_ENSURE_REGISTERED(VerifyingConsumer);

TypeId
VerifyingConsumer::GetTypeId(void)
{
  static TypeId tid =
    TypeId("ns3::ndn::VerifyingConsumer")
      .SetGroupName("Ndn")
      .SetParent<Consumer>()
      .AddConstructor<VerifyingConsumer>()
    //   .AddAttribute("StartSeq", "Initial sequence number", IntegerValue(0),
    //                 MakeIntegerAccessor(&VerifyingConsumer::m_seq), MakeIntegerChecker<int32_t>())
      //
    //   .AddAttribute("Prefix", "Name of the Interest", StringValue("/"),
    //                 MakeNameAccessor(&VerifyingConsumer::m_interestName), MakeNameChecker())
    //   .AddAttribute("LifeTime", "LifeTime for interest packet", StringValue("2s"),
    //                 MakeTimeAccessor(&VerifyingConsumer::m_interestLifeTime), MakeTimeChecker())

        .AddAttribute("Frequency", "Frequency of interest packets", StringValue("1.0"),
                      MakeDoubleAccessor(&VerifyingConsumer::m_frequency), MakeDoubleChecker<double>());

        // .AddAttribute("Randomize",
        //               "Type of send time randomization: none (default), uniform, exponential",
        //               StringValue("none"),
        //               MakeStringAccessor(&VerifyingConsumer::SetRandomize, &VerifyingConsumer::GetRandomize),
        //               MakeStringChecker())
      //
    //     .AddAttribute("MaxSeq", "Maximum sequence number to request",
    //                   IntegerValue(std::numeric_limits<uint32_t>::max()),
    //                   MakeIntegerAccessor(&VerifyingConsumer::m_seqMax), MakeIntegerChecker<uint32_t>())
      //
    //   .AddAttribute("RetxTimer",
    //                 "Timeout defining how frequent retransmission timeouts should be checked",
    //                 StringValue("50ms"),
    //                 MakeTimeAccessor(&VerifyingConsumer::GetRetxTimer, &VerifyingConsumer::SetRetxTimer),
    //                 MakeTimeChecker())
      //
    //   .AddTraceSource("LastRetransmittedInterestDataDelay",
    //                   "Delay between last retransmitted Interest and received Data",
    //                   MakeTraceSourceAccessor(&VerifyingConsumer::m_lastRetransmittedInterestDataDelay),
    //                   "ns3::ndn::VerifyingConsumer::LastRetransmittedInterestDataDelayCallback")
      //
    //   .AddTraceSource("FirstInterestDataDelay",
    //                   "Delay between first transmitted Interest and received Data",
    //                   MakeTraceSourceAccessor(&VerifyingConsumer::m_firstInterestDataDelay),
    //                   "ns3::ndn::VerifyingConsumer::FirstInterestDataDelayCallback");

  return tid;
}

VerifyingConsumer::VerifyingConsumer()
  : m_rand(CreateObject<UniformRandomVariable>())
  , m_seq(0)
  , m_seqMax(0) // don't request anything
{
  NS_LOG_FUNCTION_NOARGS();

  m_rtt = CreateObject<RttMeanDeviation>();
  m_validator = make_shared<::ndn::ValidatorConfig>();
  m_validator->load("/Users/cwood/Projects/ndnSIM/onpath/validation-config.conf");
}

void
VerifyingConsumer::ScheduleNextPacket()
{
  // double mean = 8.0 * m_payloadSize / m_desiredRate.GetBitRate ();
  // std::cout << "next: " << Simulator::Now().ToDouble(Time::S) + mean << "s\n";

  if (m_firstTime) {
    m_sendEvent = Simulator::Schedule(Seconds(0.0), &Consumer::SendPacket, this);
    m_firstTime = false;
  }
  else if (!m_sendEvent.IsRunning())
    m_sendEvent = Simulator::Schedule((m_random == 0) ? Seconds(1.0 / m_frequency)
                                                      : Seconds(m_random->GetValue()),
                                      &Consumer::SendPacket, this);
}


void
VerifyingConsumer::SetRetxTimer(Time retxTimer)
{
  m_retxTimer = retxTimer;
  if (m_retxEvent.IsRunning()) {
    // m_retxEvent.Cancel (); // cancel any scheduled cleanup events
    Simulator::Remove(m_retxEvent); // slower, but better for memory
  }

  // schedule even with new timeout
  m_retxEvent = Simulator::Schedule(m_retxTimer, &VerifyingConsumer::CheckRetxTimeout, this);
}

Time
VerifyingConsumer::GetRetxTimer() const
{
  return m_retxTimer;
}

void
VerifyingConsumer::CheckRetxTimeout()
{
  Time now = Simulator::Now();

  Time rto = m_rtt->RetransmitTimeout();
  // NS_LOG_DEBUG ("Current RTO: " << rto.ToDouble (Time::S) << "s");

  while (!m_seqTimeouts.empty()) {
    SeqTimeoutsContainer::index<i_timestamp>::type::iterator entry =
      m_seqTimeouts.get<i_timestamp>().begin();
    if (entry->time + rto <= now) // timeout expired?
    {
      uint32_t seqNo = entry->seq;
      m_seqTimeouts.get<i_timestamp>().erase(entry);
      OnTimeout(seqNo);
    }
    else
      break; // nothing else to do. All later packets need not be retransmitted
  }

  m_retxEvent = Simulator::Schedule(m_retxTimer, &VerifyingConsumer::CheckRetxTimeout, this);
}

// Application Methods
void
VerifyingConsumer::StartApplication() // Called at time specified by Start
{
  NS_LOG_FUNCTION_NOARGS();

  // do base stuff
  App::StartApplication();

  ScheduleNextPacket();
}

void
VerifyingConsumer::StopApplication() // Called at time specified by Stop
{
  NS_LOG_FUNCTION_NOARGS();

  // cancel periodic packet generation
  Simulator::Cancel(m_sendEvent);

  // cleanup base stuff
  App::StopApplication();
}

void
VerifyingConsumer::SendPacket()
{
  if (!m_active)
    return;

  NS_LOG_FUNCTION_NOARGS();

  uint32_t seq = std::numeric_limits<uint32_t>::max(); // invalid

  while (m_retxSeqs.size()) {
    seq = *m_retxSeqs.begin();
    m_retxSeqs.erase(m_retxSeqs.begin());
    break;
  }

  if (seq == std::numeric_limits<uint32_t>::max()) {
    if (m_seqMax != std::numeric_limits<uint32_t>::max()) {
      if (m_seq >= m_seqMax) {
        return; // we are totally done
      }
    }

    seq = m_seq++;
  }

  //
  shared_ptr<Name> nameWithSequence = make_shared<Name>(m_interestName);
  nameWithSequence->appendSequenceNumber(seq);
  //

  // shared_ptr<Interest> interest = make_shared<Interest> ();
  shared_ptr<Interest> interest = make_shared<Interest>();
  interest->setNonce(m_rand->GetValue(0, std::numeric_limits<uint32_t>::max()));
  interest->setName(*nameWithSequence);
  time::milliseconds interestLifeTime(m_interestLifeTime.GetMilliSeconds());
  interest->setInterestLifetime(interestLifeTime);

  // NS_LOG_INFO ("Requesting Interest: \n" << *interest);
  NS_LOG_INFO("> Interest for " << seq);

  WillSendOutInterest(seq);

  m_transmittedInterests(interest, this, m_face);
  m_face->onReceiveInterest(*interest);

  ScheduleNextPacket();
}

///////////////////////////////////////////////////
//          Process incoming packets             //
///////////////////////////////////////////////////

void
VerifyingConsumer::OnDataValidationFailed(const shared_ptr<const Data>& data,
                         const std::string& failureInfo)
  {
    std::cout << "failed" << failureInfo << std::endl;
  }

  void
  VerifyingConsumer::ValidationPassed(const ndn::shared_ptr<const ndn::Data>& data)
  {
    std::string message(reinterpret_cast<const char*>(data->getContent().value()),
                        data->getContent().value_size());
    std::cout << "msg: " << message << std::endl;
    // m_face->getIoService().stop();


    // TODO: send the alert message to the upstream router
  }

void
VerifyingConsumer::OnData(shared_ptr<const Data> data)
{
  if (!m_active)
    return;

  App::OnData(data); // tracing inside

  NS_LOG_FUNCTION(this << data);

  m_validator->validate(*data, bind(&VerifyingConsumer::ValidationPassed, this, _1),
  bind(&VerifyingConsumer::OnDataValidationFailed, this, _1, _2));

  // NS_LOG_INFO ("Received content object: " << boost::cref(*data));

  // This could be a problem......
  uint32_t seq = data->getName().at(-1).toSequenceNumber();
  NS_LOG_INFO("< DATA for " << seq);

  int hopCount = 0;
  auto ns3PacketTag = data->getTag<Ns3PacketTag>();
  if (ns3PacketTag != nullptr) { // e.g., packet came from local node's cache
    FwHopCountTag hopCountTag;
    if (ns3PacketTag->getPacket()->PeekPacketTag(hopCountTag)) {
      hopCount = hopCountTag.Get();
      NS_LOG_DEBUG("Hop count: " << hopCount);
    }
  }

  SeqTimeoutsContainer::iterator entry = m_seqLastDelay.find(seq);
  if (entry != m_seqLastDelay.end()) {
    m_lastRetransmittedInterestDataDelay(this, seq, Simulator::Now() - entry->time, hopCount);
  }

  entry = m_seqFullDelay.find(seq);
  if (entry != m_seqFullDelay.end()) {
    m_firstInterestDataDelay(this, seq, Simulator::Now() - entry->time, m_seqRetxCounts[seq], hopCount);
  }

  m_seqRetxCounts.erase(seq);
  m_seqFullDelay.erase(seq);
  m_seqLastDelay.erase(seq);

  m_seqTimeouts.erase(seq);
  m_retxSeqs.erase(seq);

  m_rtt->AckSeq(SequenceNumber32(seq));
}

void
VerifyingConsumer::OnTimeout(uint32_t sequenceNumber)
{
  NS_LOG_FUNCTION(sequenceNumber);
  // std::cout << Simulator::Now () << ", TO: " << sequenceNumber << ", current RTO: " <<
  // m_rtt->RetransmitTimeout ().ToDouble (Time::S) << "s\n";

  m_rtt->IncreaseMultiplier(); // Double the next RTO
  m_rtt->SentSeq(SequenceNumber32(sequenceNumber),
                 1); // make sure to disable RTT calculation for this sample
  m_retxSeqs.insert(sequenceNumber);
  ScheduleNextPacket();
}

void
VerifyingConsumer::WillSendOutInterest(uint32_t sequenceNumber)
{
  NS_LOG_DEBUG("Trying to add " << sequenceNumber << " with " << Simulator::Now() << ". already "
                                << m_seqTimeouts.size() << " items");

  m_seqTimeouts.insert(SeqTimeout(sequenceNumber, Simulator::Now()));
  m_seqFullDelay.insert(SeqTimeout(sequenceNumber, Simulator::Now()));

  m_seqLastDelay.erase(sequenceNumber);
  m_seqLastDelay.insert(SeqTimeout(sequenceNumber, Simulator::Now()));

  m_seqRetxCounts[sequenceNumber]++;

  m_rtt->SentSeq(SequenceNumber32(sequenceNumber), 1);
}



} // namespace ndn
} // namespace ns3

#include "threads_commutator.hpp"

#include "message_acceptor.hpp"

#include "../base/assert.hpp"

namespace df
{
  void ThreadsCommutator::RegisterThread(ThreadName name, MessageAcceptor * acceptor)
  {
    ASSERT(m_acceptors.find(name) == m_acceptors.end(), ());
    m_acceptors[name] = acceptor;
  }

  void ThreadsCommutator::PostMessage(ThreadName name, Message * message)
  {
    acceptors_map_t::iterator it = m_acceptors.find(name);
    ASSERT(it != m_acceptors.end(), ());
    if (it != m_acceptors.end())
      it->second->AcceptMessage(message);
  }
}


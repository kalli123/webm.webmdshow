// Copyright (c) 2010 The WebM project authors. All Rights Reserved.
//
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file in the root of the source
// tree. An additional intellectual property rights grant can be found
// in the file PATENTS.  All contributing project authors may
// be found in the AUTHORS file in the root of the source tree.

#include <strmif.h>
#include "mkvparserstream.hpp"
#include "mkvparser.hpp"
#include <cassert>
#include <sstream>
#include <iomanip>
#include <vfwmsgs.h>
#ifdef _DEBUG
#include "odbgstream.hpp"
using std::endl;
#endif

namespace mkvparser
{


Stream::Stream(const Track* pTrack) :
    m_pTrack(pTrack)
{
    Init();
}


Stream::~Stream()
{
}


void Stream::Init()
{
    m_pBase = 0;
    m_pCurr = 0;  //lazy init this later
    m_pStop = m_pTrack->GetEOS();  //means play entire stream
    m_bDiscontinuity = true;
}


std::wstring Stream::GetId() const
{
    std::wostringstream os;

    GetKind(os);  //"Video" or "Audio"

    os << std::setw(3) << std::setfill(L'0') << m_pTrack->GetNumber();

    return os.str();
}


std::wstring Stream::GetName() const
{
    const Track* const t = m_pTrack;
    assert(t);

    if (const char* codecName = t->GetCodecNameAsUTF8())
        return ConvertFromUTF8(codecName);

    if (const char* name = t->GetNameAsUTF8())
        return ConvertFromUTF8(name);

    if (LONGLONG tn = t->GetNumber())
    {
        std::wostringstream os;
        os << L"Track" << tn;
        return os.str();
    }

    if (const char* codecId = t->GetCodecId())
    {
        std::wstring result;

        const char* p = codecId;

        while (*p)
            result += wchar_t(*p++);  //TODO: is this cast meaningful?

        return result;
    }

    return GetId();
}


__int64 Stream::GetDuration() const
{
    Segment* const pSegment = m_pTrack->m_pSegment;
    assert(pSegment);

    const __int64 ns = pSegment->GetDuration();  //scaled (nanoseconds)
    assert(ns >= 0);

    const __int64 d = ns / 100;  //100-ns ticks

    return d;
}


HRESULT Stream::GetAvailable(LONGLONG* pLatest) const
{
    if (pLatest == 0)
        return E_POINTER;

    LONGLONG& pos = *pLatest;  //units are current time format

    Segment* const pSegment = m_pTrack->m_pSegment;

    if (pSegment->Unparsed() <= 0)
        pos = GetDuration();
    else
    {
        const Cluster* const pCluster = pSegment->GetLast();

        if ((pCluster == 0) || pCluster->EOS())
            pos = 0;
        else
        {
            const __int64 ns = pCluster->GetTime();
            pos = ns / 100;  //TODO: reftime units are assumed here
        }
    }

    return S_OK;
}


__int64 Stream::GetCurrPosition() const
{
    return GetCurrTime();  //TODO: for now only support reftime units
}


__int64 Stream::GetCurrTime() const
{
    if (m_pCurr == 0)  //NULL means lazy init hasn't happened yet
        return 0;      //TODO: assumes track starts with t=0

    if (m_pCurr->EOS())
        return GetDuration();

    const Block* const pBlock = m_pCurr->GetBlock();
    assert(pBlock);

    //TODO: This is not quite right for the B-frame case.  DirectShow
    //sort of assumes media times always increase, but that's not the
    //case for B-frames.  In fact for B-Frames we don't even set
    //the media time.  Ideally we shouldn't even see a B-frame (when we
    //populate a media sample, just send the B-frames as part of
    //the same sample as the preceding I- or P-frame).
    const __int64 ns = pBlock->GetTime(m_pCurr->GetCluster());
    const __int64 reftime = ns / 100;  //100-ns ticks

    return reftime;
}


__int64 Stream::GetStopPosition() const
{
    return GetStopTime();  //TODO: for now we only support reftime units
}


__int64 Stream::GetStopTime() const
{
    if (m_pStop == 0)  //interpreted to mean "play to end of stream"
        return GetDuration();

    if (m_pStop->EOS())
        return GetDuration();

    const Block* const pBlock = m_pStop->GetBlock();
    assert(pBlock);

    const __int64 ns = pBlock->GetTime(m_pStop->GetCluster());
    const __int64 reftime = ns / 100;  //100-ns ticks

    return reftime;
}


LONGLONG Stream::GetSeekTime(
    LONGLONG currpos_reftime,
    DWORD dwCurr_) const
{
    const DWORD dwCurrPos = dwCurr_ & AM_SEEKING_PositioningBitsMask;
    assert(dwCurrPos != AM_SEEKING_NoPositioning);  //handled by caller

    Segment* const pSegment = m_pTrack->m_pSegment;

    const __int64 duration_ns = pSegment->GetDuration();
    assert(duration_ns >= 0);

    const __int64 currpos_ns = currpos_reftime * 100;
    __int64 tCurr_ns;

    switch (dwCurrPos)
    {
        case AM_SEEKING_IncrementalPositioning:  //applies only to stop pos
        default:
            assert(false);
            return 0;

        case AM_SEEKING_AbsolutePositioning:
        {
            tCurr_ns = currpos_ns;
            break;
        }
        case AM_SEEKING_RelativePositioning:
        {
            if (m_pCurr == 0)
                tCurr_ns = currpos_ns;  //t=0 is assumed here
            else if (m_pCurr->EOS())
                tCurr_ns = duration_ns + currpos_ns;
            else
            {
                const Block* const pBlock = m_pCurr->GetBlock();
                assert(pBlock);

                tCurr_ns = pBlock->GetTime(m_pCurr->GetCluster()) + currpos_ns;
            }

            break;
        }
    }

    return tCurr_ns;
}


const Cluster* Stream::GetSeekBase(LONGLONG tCurr_ns, bool use_cues) const
{
    Segment* const pSegment = m_pTrack->m_pSegment;

    if (pSegment->GetCount() == 0)
    {
        if (pSegment->Unparsed() <= 0)
            return &pSegment->m_eos;

        return 0;  //lazy init later when we have data
    }

    if (tCurr_ns <= 0)
        return 0;

    if (tCurr_ns >= pSegment->GetDuration())
        return &pSegment->m_eos;

    if (!use_cues)
        __noop;
    else if (const Cues* pCues = pSegment->GetCues())
    {
        const mkvparser::CuePoint* pCP;
        const mkvparser::CuePoint::TrackPosition* pTP;

        const bool bFound = pCues->Find(tCurr_ns, m_pTrack, pCP, pTP);

        if (bFound)
        {
            const BlockEntry* const pBE = pCues->GetBlock(pCP, pTP);
            assert(pBE);
            assert(!pBE->EOS());

            return pBE->GetCluster();
        }

        //yes, fall through
    }

    //TODO: use Segment::FindCluster instead?
    const BlockEntry* const pBE = pSegment->Seek(tCurr_ns, m_pTrack);
    assert(pBE);
    assert(!pBE->EOS());

    return pBE->GetCluster();
}


#if 0
void Stream::PreloadSeek(LONGLONG seek_ns)
{
    Segment* const pSegment = m_pTrack->m_pSegment;

    const Cues* const pCues = pSegment->GetCues();

    if (pCues == 0)
        return;  //TODO: try a diffent method of pre-loading clusters

    if (pSegment->Unparsed() <= 0)
        return;  //nothing needs to be done

    HRESULT hr = Preload();  //ensure we have at least one cluster

    Cluster* pCluster = pSegment->GetLast();
    assert(pCluster);

    if (pCluster->EOS())
        return;  //weird: no clusters

    const LONGLONG last_ns = pCluster->GetTime();

    if (seek_ns <= last_ns)
        return;  //nothing else needs to be done

    const CuePoint* pCP;
    const CuePoint::TrackPosition* pTP;

    if (!pCues->FindNext(last_ns, m_pTrack, pCP, pTP))
        return;

    assert(pCP);
    assert(pTP);

    while (pSegment->Unparsed() > 0)
    {
        hr = Preload();

        pCluster = pSegment->GetLast();
        assert(pCluster);
        assert(!pCluster->EOS());

        const __int64 pos = _abs64(pCluster->m_pos);

        if (pos >= pTP->m_pos)
            return;
    }
}
#endif


const Cluster* Stream::Seek(LONGLONG tCurr_ns, bool use_cues)
{
    Segment* const pSegment = m_pTrack->m_pSegment;

    const __int64 duration_ns = pSegment->GetDuration();
    assert(duration_ns >= 0);

    m_bDiscontinuity = true;

    if (pSegment->GetCount() == 0)
    {
        //TODO: we can probably do better here.

        if (pSegment->Unparsed() <= 0)
        {
            m_pBase = &pSegment->m_eos;
            m_pCurr = m_pTrack->GetEOS();
        }
        else
        {
            m_pBase = 0;
            m_pCurr = 0;  //lazy init later when we have data
        }

        return m_pBase;
    }

    if (tCurr_ns <= 0)
    {
        m_pBase = 0;
        m_pCurr = 0;  //lazy init later

        return m_pBase;
    }

    if (tCurr_ns >= duration_ns)
    {
        m_pBase = &pSegment->m_eos;
        m_pCurr = m_pTrack->GetEOS();

        return m_pBase;
    }

    if (!use_cues)
        __noop;
    else if (const Cues* pCues = pSegment->GetCues())
    {
        const mkvparser::CuePoint* pCP;
        const mkvparser::CuePoint::TrackPosition* pTP;

        const bool bFound = pCues->Find(tCurr_ns, m_pTrack, pCP, pTP);

        if (bFound)
        {
            m_pCurr = pCues->GetBlock(pCP, pTP);
            assert(m_pCurr);
            assert(!m_pCurr->EOS());

            m_pBase = m_pCurr->GetCluster();
            return m_pBase;
        }
    }

    //TODO: use Segment::FindCluster instead?
    m_pCurr = pSegment->Seek(tCurr_ns, m_pTrack);
    assert(m_pCurr);
    assert(!m_pCurr->EOS());

    m_pBase = m_pCurr->GetCluster();
    return m_pBase;
}


void Stream::SetCurrPosition(const Cluster* pBase)
{
    if (pBase == 0)
        m_pCurr = 0;  //lazy init
    else
        m_pCurr = pBase->GetEntry(m_pTrack);

    m_pBase = pBase;
    m_bDiscontinuity = true;
}


void Stream::SetStopPosition(
    LONGLONG stoppos_reftime,
    DWORD dwStop_)
{
    const DWORD dwStopPos = dwStop_ & AM_SEEKING_PositioningBitsMask;
    assert(dwStopPos != AM_SEEKING_NoPositioning);  //handled by caller

    Segment* const pSegment = m_pTrack->m_pSegment;

    if (pSegment->GetCount() == 0)
    {
        m_pStop = m_pTrack->GetEOS();  //means "play to end"
        return;
    }

    if ((m_pCurr != 0) && m_pCurr->EOS())
    {
        m_pStop = m_pTrack->GetEOS();
        return;
    }

    __int64 tCurr_ns;

    if (m_pCurr == 0)  //lazy init
        tCurr_ns = 0;  //nanoseconds
    else
    {
        const Block* const pBlock = m_pCurr->GetBlock();

        tCurr_ns = pBlock->GetTime(m_pCurr->GetCluster());
        assert(tCurr_ns >= 0);
    }

    const Cluster* const pFirst = pSegment->GetFirst();
    const Cluster* const pCurrCluster = m_pBase ? m_pBase : pFirst;
    pCurrCluster;
    assert(pCurrCluster);
    assert(!pCurrCluster->EOS());
    assert(tCurr_ns >= pCurrCluster->GetTime());

    const __int64 duration_ns = pSegment->GetDuration();
    assert(duration_ns >= 0);

    const __int64 stoppos_ns = stoppos_reftime * 100;
    __int64 tStop_ns;

    switch (dwStopPos)
    {
        default:
            assert(false);
            return;

        case AM_SEEKING_AbsolutePositioning:
        {
            tStop_ns = stoppos_reftime;
            break;
        }
        case AM_SEEKING_RelativePositioning:
        {
            if ((m_pStop == 0) || m_pStop->EOS())
                tStop_ns = duration_ns + stoppos_ns;
            else
            {
                const Block* const pBlock = m_pStop->GetBlock();
                assert(pBlock);

                tStop_ns = pBlock->GetTime(m_pStop->GetCluster()) + stoppos_ns;
            }

            break;
        }
        case AM_SEEKING_IncrementalPositioning:
        {
            if (stoppos_reftime <= 0)
            {
                m_pStop = m_pCurr;
                return;
            }

            tStop_ns = tCurr_ns + stoppos_ns;
            break;
        }
    }

    if (tStop_ns <= tCurr_ns)
    {
        m_pStop = m_pCurr;
        return;
    }

    if (tStop_ns >= duration_ns)
    {
        m_pStop = m_pTrack->GetEOS();
        return;
    }

    const Cluster* pStopCluster = pSegment->FindCluster(tStop_ns);  //TODO
    assert(pStopCluster);

    if (pStopCluster == pCurrCluster)
        pStopCluster = pSegment->GetNext(pStopCluster);

    m_pStop = pStopCluster->GetEntry(m_pTrack);
    assert((m_pStop == 0) ||
           m_pStop->EOS() ||
           (m_pStop->GetBlock()->GetTime(m_pStop->GetCluster()) >= tCurr_ns));
}


void Stream::SetStopPositionEOS()
{
    m_pStop = m_pTrack->GetEOS();
}


HRESULT Stream::Preload()
{
    Segment* const pSegment = m_pTrack->m_pSegment;

#if 0
    Cluster* pCluster;
    __int64 pos;

    const HRESULT hr = pSegment->ParseCluster(pCluster, pos);
    hr;
    assert(SUCCEEDED(hr));  //file-based load is assumed here

    const bool bDone = pSegment->AddCluster(pCluster, pos);

    return bDone ? S_FALSE : S_OK;
#else
    const int status = pSegment->LoadCluster();

    if (status < 0)  //error
        return E_FAIL;

    return S_OK;
#endif
}


HRESULT Stream::PopulateSample(IMediaSample* pSample)
{
    if (pSample == 0)
        return E_INVALIDARG;

    if (SendPreroll(pSample))
        return S_OK;

    if (m_pCurr == 0)  //lazy-init of first block
    {
        const long status = m_pTrack->GetFirst(m_pCurr);

        if (status == E_BUFFER_NOT_FULL)
            return VFW_E_BUFFER_UNDERFLOW;

        assert(status >= 0);  //success
        assert(m_pCurr);

        m_pBase = m_pTrack->m_pSegment->GetFirst();
        assert(m_pBase);
    }

    if (m_pStop == 0)  //TODO: this test might not be req'd
    {
        if (m_pCurr->EOS())
            return S_FALSE;  //send EOS downstream
    }
    else if (m_pCurr == m_pStop)
    {
        return S_FALSE;  //EOS
    }

    const BlockEntry* pNextBlock;

    long status = m_pTrack->GetNext(m_pCurr, pNextBlock);

    if (status == E_BUFFER_NOT_FULL)
        return VFW_E_BUFFER_UNDERFLOW;

    assert(status >= 0);  //success
    assert(pNextBlock);

#if 0  //TODO: keep this?
    const BlockEntry* pNextTime;

    status = m_pTrack->GetNextTime(m_pCurr, pNextBlock, pNextTime);

    if (status == E_BUFFER_NOT_FULL)
        return VFW_E_BUFFER_UNDERFLOW;

    assert(status >= 0);  //success
    assert(pNextTime);
#endif

    const HRESULT hr = OnPopulateSample( /* pNextTime */ pNextBlock, pSample);
    assert(SUCCEEDED(hr));  //TODO

    m_pCurr = pNextBlock;

    if (hr != S_OK)  //TODO: do we still need to do this?
        return 2;    //throw away this sample

    m_bDiscontinuity = false;

    return S_OK;
}


bool Stream::SendPreroll(IMediaSample*)
{
    return false;
}


ULONG Stream::GetClusterCount() const
{
    return m_pTrack->m_pSegment->GetCount();
}


HRESULT Stream::SetConnectionMediaType(const AM_MEDIA_TYPE&)
{
    return S_OK;
}


std::wstring Stream::ConvertFromUTF8(const char* str)
{
    const int cch = MultiByteToWideChar(
                        CP_UTF8,
                        0,  //TODO: MB_ERR_INVALID_CHARS
                        str,
                        -1,  //include NUL terminator in result
                        0,
                        0);  //request length

    assert(cch > 0);

    const size_t cb = cch * sizeof(wchar_t);
    wchar_t* const wstr = (wchar_t*)_malloca(cb);

    const int cch2 = MultiByteToWideChar(
                        CP_UTF8,
                        0,  //TODO: MB_ERR_INVALID_CHARS
                        str,
                        -1,
                        wstr,
                        cch);

    cch2;
    assert(cch2 > 0);
    assert(cch2 == cch);

    return wstr;
}


}  //end namespace mkvparser

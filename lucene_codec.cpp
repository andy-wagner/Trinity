#include "lucene_codec.h"
#include "utils.h"
#include <ansifmt.h>
#include <switch_bitops.h>
#ifdef LUCENE_USE_STREAMVBYTE
#include <ext/streamvbyte/include/streamvbyte.h>
#include <ext/streamvbyte/include/streamvbytedelta.h>
#elif defined(LUCENE_USE_MASKEDVBYTE)
#include <ext/MaskedVByte/include/varintdecode.h>
#include <ext/MaskedVByte/include/varintencode.h>
#endif

static constexpr bool trace{false};


static bool all_equal(const uint32_t *const __restrict__ values, const size_t n) noexcept
{
        const auto v = values[0];

        for (uint32_t i{1}; i < n; ++i)
        {
                if (values[i] != v)
                        return false;
        }
        return true;
}

#ifdef LUCENE_USE_FASTPFOR
static void ints_encode(FastPForLib::FastPFor<4> &forUtil, const uint32_t *values, const size_t n, IOBuffer &out)
#else
static void ints_encode(const uint32_t *values, const size_t n, IOBuffer &out)
#endif
{
        if (all_equal(values, n))
        {
                if (trace)
                        SLog("ENCODING all equal ", values[0], "\n");

                out.pack(uint8_t(0));
                out.encode_varbyte32(values[0]);
                return;
        }

#ifdef LUCENE_USE_STREAMVBYTE
        out.reserve(n * 8 + 256);
        out.pack(uint8_t(1));

        const auto len = streamvbyte_encode(const_cast<uint32_t *>(values), n, reinterpret_cast<uint8_t *>(out.end()));

        out.advance_size(len);

#elif defined(LUCENE_USE_MASKEDVBYTE)
        out.reserve(n * 8);
        out.pack(uint8_t(1));

        const auto len = vbyte_encode(const_cast<uint32_t *>(values), n, (uint8_t *)out.end());

        out.advance_size(len);
#else
        const auto offset = out.size();

        out.RoomFor(sizeof(uint8_t));
        out.reserve((n + n) * sizeof(uint32_t));
        auto l = out.capacity() / sizeof(uint32_t);
        forUtil.encodeArray(values, n, (uint32_t *)out.end(), l);
        out.advance_size(l * sizeof(uint32_t));
        *(out.data() + offset) = l; // this is great, means we can skip ahead n * sizeof(uint32_t) bytes to get to the next block
#endif
}

#ifdef LUCENE_USE_FASTPFOR
static const uint8_t *ints_decode(FastPForLib::FastPFor<4> &forUtil, const uint8_t *__restrict p, uint32_t *const __restrict values)
#else
static const uint8_t *ints_decode(const uint8_t *__restrict p, uint32_t *const __restrict values)
#endif
{
        if (const auto blockSize = *p++; blockSize == 0)
        {
                // all equal values
                uint32_t value;

                varbyte_get32(p, value);

                if (trace)
                        SLog("All equal values of ", value, "\n");

                for (uint32_t i{0}; i != Trinity::Codecs::Lucene::BLOCK_SIZE; ++i)
                        values[i] = value;
        }
        else
        {
#ifdef LUCENE_USE_STREAMVBYTE
                p += streamvbyte_decode(p, values, Trinity::Codecs::Lucene::BLOCK_SIZE);
#elif defined(LUCENE_USE_MASKEDVBYTE)
                p += masked_vbyte_decode(p, values, Trinity::Codecs::Lucene::BLOCK_SIZE);
#else
                size_t n{Trinity::Codecs::Lucene::BLOCK_SIZE};
                const auto *ptr = reinterpret_cast<const uint32_t *>(p);

                ptr = forUtil.decodeArray(ptr, blockSize, values, n);
                p = reinterpret_cast<const uint8_t *>(ptr);
#endif
        }

        return p;
}

void Trinity::Codecs::Lucene::IndexSession::begin()
{
        // We will need two extra/additional buffers, one for documents, another for the hits
        // TODO: we really need to do the right thing here, reset etc
}

void Trinity::Codecs::Lucene::IndexSession::flush_positions_data()
{
        if (positionsOutFd == -1)
        {
                positionsOutFd = open(Buffer{}.append(basePath, "/hits.data.t").c_str(), O_WRONLY | O_LARGEFILE | O_CREAT, 0775);

                if (positionsOutFd == -1)
                        throw Switch::data_error("Failed to persist hits.data");
        }

        if (Utilities::to_file(positionsOut.data(), positionsOut.size(), positionsOutFd) == -1)
                throw Switch::data_error("Failed to persist hits.data");

        positionsOutFlushed += positionsOut.size();
        positionsOut.clear();
}

void Trinity::Codecs::Lucene::IndexSession::end()
{
        if (positionsOut.size())
                flush_positions_data();

        if (positionsOutFd != -1)
        {
                if (close(positionsOutFd) == -1)
                        throw Switch::data_error("Failed to persist hits.data");

                positionsOutFd = -1;

                if (rename(Buffer{}.append(basePath, "/hits.data.t").c_str(), Buffer{}.append(basePath, "/hits.data").c_str()) == -1)
                {
                        unlink(Buffer{}.append(basePath, "/hits.data.t").c_str());
                        throw Switch::data_error("Failed to persist hits.data");
                }
        }
}

range32_t Trinity::Codecs::Lucene::IndexSession::append_index_chunk(const Trinity::Codecs::AccessProxy *src_, const term_index_ctx srcTCTX)
{
        const auto src = static_cast<const Trinity::Codecs::Lucene::AccessProxy *>(src_);
        const auto o = indexOut.size() + indexOutFlushed;

        require(srcTCTX.indexChunk.size());

        auto *p = src->indexPtr + srcTCTX.indexChunk.offset, *const end = p + srcTCTX.indexChunk.size();
        const auto hitsDataOffset = *(uint32_t *)p;
        p += sizeof(uint32_t);
        const auto sumHits = *(uint32_t *)p;
        p += sizeof(uint32_t);
        const auto positionsChunkSize = *(uint32_t *)p;
        p += sizeof(uint32_t);
        [[maybe_unused]] const auto skiplistSize = *(uint16_t *)p;
        p += sizeof(uint16_t);
        const auto newHitsDataOffset = positionsOut.size() + positionsOutFlushed;

        positionsOut.serialize(src->hitsDataPtr + hitsDataOffset, positionsChunkSize);
        indexOut.pack(uint32_t(newHitsDataOffset), sumHits, positionsChunkSize, skiplistSize);
        indexOut.serialize(p, end - p);

        return {uint32_t(o), srcTCTX.indexChunk.size()};
}

void Trinity::Codecs::Lucene::Encoder::begin_term()
{
        const auto s = static_cast<Trinity::Codecs::Lucene::IndexSession *>(sess);

        lastDocID = 0;
        totalHits = 0;
        sumHits = 0;
        buffered = 0;
        termDocuments = 0;
        termIndexOffset = sess->indexOut.size() + sess->indexOutFlushed;
        termPositionsOffset = s->positionsOut.size() + s->positionsOutFlushed;
        lastHitsBlockOffset = 0;
        lastHitsBlockTotalHits = 0;
        skiplistCountdown = SKIPLIST_STEP;
        skiplist.clear();

        sess->indexOut.pack(uint32_t(termPositionsOffset), uint32_t(0), uint32_t(0), uint16_t(0)); // will fill in later. Will also track positions chunk size for efficient merge
}

void Trinity::Codecs::Lucene::Encoder::output_block()
{
        if (trace)
                SLog("<< BLOCK\n");

        require(buffered == BLOCK_SIZE);

        if (--skiplistCountdown == 0)
        {
                if (likely(skiplist.size() < UINT16_MAX))
                {
                        // keep it sane
                        skiplist.push_back(cur_block);
                }
                skiplistCountdown = SKIPLIST_STEP;
        }

        auto indexOut = &sess->indexOut;

#ifdef LUCENE_USE_FASTPFOR
        ints_encode(forUtil, docDeltas, buffered, *indexOut);
        ints_encode(forUtil, docFreqs, buffered, *indexOut);
#else
        ints_encode(docDeltas, buffered, *indexOut);
        ints_encode(docFreqs, buffered, *indexOut);
#endif
        buffered = 0;

        if (trace)
                SLog("Encoded now ", indexOut->size() + sess->indexOutFlushed, "\n");
}

void Trinity::Codecs::Lucene::Encoder::begin_document(const uint32_t documentID)
{
        require(documentID > lastDocID);

        if (trace)
                SLog("INDEXING document ", documentID, "\n");

        if (unlikely(buffered == BLOCK_SIZE))
                output_block();

        if (!buffered)
        {
                cur_block.indexOffset = (sess->indexOut.size() + sess->indexOutFlushed) - termIndexOffset;
                cur_block.lastDocID = lastDocID; // last block's last document ID
                cur_block.totalDocumentsSoFar = termDocuments;

                // Last positions/hits block when this new block is captured
                cur_block.lastHitsBlockOffset = lastHitsBlockOffset;
                cur_block.lastHitsBlockTotalHits = lastHitsBlockTotalHits;

                // total hits of the current position/hits block
                cur_block.curHitsBlockHits = totalHits;
        }

        docDeltas[buffered] = documentID - lastDocID;
        docFreqs[buffered] = 0;
        ++termDocuments;

        lastDocID = documentID;
        lastPosition = 0;
}

void Trinity::Codecs::Lucene::Encoder::new_hit(const uint32_t pos, const range_base<const uint8_t *, const uint8_t> payload)
{
        static constexpr bool trace{false};
        //const bool trace = lastDocID== 2151228176 || lastDocID== 2152925656 || lastDocID==  2154895013;

        if (trace)
                SLog("New hit (", pos, ", ", payload, ")\n");

        if (!pos && !payload)
        {
                // This is perfectly fine
                return;
        }

        require(pos >= lastPosition);

        const auto delta = pos - lastPosition;

        ++docFreqs[buffered];
        hitPosDeltas[totalHits] = delta;
        hitPayloadSizes[totalHits] = payload.size();
        lastPosition = pos;

        if (const auto size = payload.size())
        {
                require(size <= sizeof(uint64_t));
                payloadsBuf.serialize(payload.offset, size);
        }

        ++totalHits;
        if (unlikely(totalHits == BLOCK_SIZE))
        {
                auto s = static_cast<Trinity::Codecs::Lucene::IndexSession *>(sess);
                auto positionsOut = &s->positionsOut;

                sumHits += totalHits;

#ifdef LUCENE_USE_FASTPFOR
                ints_encode(forUtil, hitPosDeltas, totalHits, *positionsOut);
                ints_encode(forUtil, hitPayloadSizes, totalHits, *positionsOut);
#else
                ints_encode(hitPosDeltas, totalHits, *positionsOut);
                ints_encode(hitPayloadSizes, totalHits, *positionsOut);
#endif

                {
                        size_t s{0};

                        for (uint32_t i{0}; i != totalHits; ++i)
                                s += hitPayloadSizes[i];

                        require(s == payloadsBuf.size());
                }

                if (trace)
                        SLog("<< payloads length:", payloadsBuf.size(), "\n");

                positionsOut->encode_varbyte32(payloadsBuf.size());
                positionsOut->serialize(payloadsBuf.data(), payloadsBuf.size());
                payloadsBuf.clear();

                lastHitsBlockTotalHits = sumHits;
                lastHitsBlockOffset = (positionsOut->size() + s->positionsOutFlushed) - termPositionsOffset;

                totalHits = 0;
        }
}

void Trinity::Codecs::Lucene::Encoder::end_document()
{
        ++buffered;
}

void Trinity::Codecs::Lucene::Encoder::end_term(term_index_ctx *out)
{
        auto indexOut = &sess->indexOut;
        auto *const __restrict__ s = static_cast<Trinity::Codecs::Lucene::IndexSession *>(sess);

        sumHits += totalHits;

        if (trace)
                SLog("Remaining ", buffered, " (sumHits = ", sumHits, ")\n");

        if (buffered == BLOCK_SIZE)
                output_block();
        else
        {
                for (uint32_t i{0}; i != buffered; ++i)
                {
                        const auto delta = docDeltas[i];
                        const auto freq = docFreqs[i];

#if defined(LUCENE_ENCODE_FREQ1_DOCDELTA)
                        if (freq == 1)
                                indexOut->encode_varbyte32((delta << 1) | 1);
                        else
                        {
                                indexOut->encode_varbyte32(delta << 1);
                                indexOut->encode_varbyte32(freq);
                        }
#else
                        indexOut->encode_varbyte32(delta);
                        indexOut->encode_varbyte32(freq);
#endif
                }
        }

        *(uint32_t *)(sess->indexOut.data() + (termIndexOffset - sess->indexOutFlushed) + sizeof(uint32_t)) = sumHits;

        if (totalHits)
        {
                uint8_t lastPayloadLen{0x0};
                auto positionsOut = &s->positionsOut;
                size_t sum{0};

                for (uint32_t i{0}; i != totalHits; ++i)
                {
                        const auto posDelta = hitPosDeltas[i];
                        const auto payloadLen = hitPayloadSizes[i];

                        if (payloadLen != lastPayloadLen)
                        {
                                lastPayloadLen = payloadLen;
                                positionsOut->encode_varbyte32((posDelta << 1) | 1);
                                positionsOut->pack(uint8_t(payloadLen));
                        }
                        else
                                positionsOut->encode_varbyte32(posDelta << 1);

                        sum += payloadLen;
                }

                // we don't need to encode as varbyte the payloadsBuf.size() because
                // we can just sum those individual hit payload lengths
                require(sum == payloadsBuf.size());
                positionsOut->serialize(payloadsBuf.data(), payloadsBuf.size());
                payloadsBuf.clear();
        }

        const uint16_t skiplistSize = skiplist.size();

        *(uint32_t *)(sess->indexOut.data() + (termIndexOffset - sess->indexOutFlushed) + sizeof(uint32_t) + sizeof(uint32_t)) = (s->positionsOut.size() + s->positionsOutFlushed) - termPositionsOffset;
        *(uint16_t *)(sess->indexOut.data() + (termIndexOffset - sess->indexOutFlushed) + sizeof(uint32_t) + sizeof(uint32_t) + sizeof(uint32_t)) = skiplistSize;

        if (skiplistSize)
        {
                // serialize skiplist here
                auto *const __restrict__ b = &sess->indexOut;

                for (const auto &it : skiplist)
                        b->pack(it.indexOffset, it.lastDocID, it.lastHitsBlockOffset, it.totalDocumentsSoFar, it.lastHitsBlockTotalHits, it.curHitsBlockHits);

                skiplist.clear();
        }

        out->documents = termDocuments;
        out->indexChunk.Set(termIndexOffset, uint32_t((sess->indexOut.size() + sess->indexOutFlushed) - termIndexOffset));

        if (const auto f = s->flushFreq; f && unlikely(s->positionsOut.size() > f))
                s->flush_positions_data();
}

Trinity::Codecs::Encoder *Trinity::Codecs::Lucene::IndexSession::new_encoder()
{
        return new Trinity::Codecs::Lucene::Encoder(this);
}

Trinity::Codecs::Decoder *Trinity::Codecs::Lucene::AccessProxy::new_decoder(const term_index_ctx &tctx)
{
        auto d = std::make_unique<Trinity::Codecs::Lucene::Decoder>();

        d->init(tctx, this);
        return d.release();
}

void Trinity::Codecs::Lucene::Decoder::refill_hits(PostingsListIterator *it)
{
        uint32_t payloadsChunkLength;

        if (it->hitsLeft >= BLOCK_SIZE)
        {
#ifdef LUCENE_USE_FASTPFOR
                it->hdp = ints_decode(forUtil, it->hdp, it->hitsPositionDeltas);
                it->hdp = ints_decode(forUtil, it->hdp, it->hitsPayloadLengths);
#else
                it->hdp = ints_decode(it->hdp, it->hitsPositionDeltas);
                it->hdp = ints_decode(it->hdp, it->hitsPayloadLengths);
#endif

                varbyte_get32(it->hdp, payloadsChunkLength);


                it->payloadsIt = it->hdp;
                it->hdp += payloadsChunkLength;
                it->payloadsEnd = it->hdp;

                it->bufferedHits = BLOCK_SIZE;
                it->hitsLeft -= BLOCK_SIZE;


        }
        else
        {
                uint32_t v;
                uint8_t payloadLen{0};

                payloadsChunkLength = 0;
                for (uint32_t i{0}; i != it->hitsLeft; ++i)
                {
                        varbyte_get32(it->hdp, v);

                        if (v & 1)
                        {
                                payloadLen = *(it->hdp++);
                        }

                        it->hitsPositionDeltas[i] = v >> 1;
                        it->hitsPayloadLengths[i] = payloadLen;
                        payloadsChunkLength += payloadLen;
                }
                it->payloadsIt = it->hdp;
                it->hdp += payloadsChunkLength;
                it->payloadsEnd = it->hdp;
                it->bufferedHits = it->hitsLeft;
                it->hitsLeft = 0;
        }
        it->hitsIndex = 0;

#if 0
        {
                auto p{it->payloadsIt};

                for (uint32_t i{0}; i != it->bufferedHits; ++i)
                {
                        const auto len = it->hitsPayloadLengths[i];
                        uint64_t h;

                        memcpy(&h, p, len);
                        p += len;

                        SLog("FOR ", i, " ", len, " ", len ? h : 0, "\n");
                }
        }
#endif
}

[[gnu::hot]] void Trinity::Codecs::Lucene::Decoder::skip_hits(Trinity::Codecs::Lucene::PostingsListIterator *it, const uint32_t n)
{
        if (auto rem = n)
        {
                auto &hitsPayloadLengths{it->hitsPayloadLengths};

                do
                {
                        if (it->hitsIndex + rem == it->bufferedHits)
                        {
                                // fast-path TODO: verify me(this works, but hm.)
                                it->skippedHits -= rem;
                                it->hitsIndex = 0;
                                it->bufferedHits = 0;
                                return;
                        }

                        if (it->hitsIndex == it->bufferedHits)
                        {
                                refill_hits(it);
                        }

                        const auto step = std::min<uint32_t>(rem, it->bufferedHits - it->hitsIndex);

#if defined(TRINITY_ENABLE_PREFETCH)
                        {
                                const size_t prefetchIterations = step / 16; // 64/4
                                const auto end = it->hitsIndex + step;

                                for (uint32_t i{0}; i != prefetchIterations; ++i)
                                {
                                        _mm_prefetch(hitsPayloadLengths + hitsIndex, _MM_HINT_NTA);

                                        for (const auto upto = hitsIndex + 16; hitsIndex != upto; ++(it->hitsIndex))
                                                payloadsIt += hitsPayloadLengths[it->hitsIndex];
                                }

                                while (it->hitsIndex != end)
                                        it->payloadsIt += hitsPayloadLengths[it->hitsIndex++];
                        }
#else
                        uint32_t sum{0};
                        auto hitsIndex{it->hitsIndex};

                        for (uint32_t i{0}; i != step; ++i)
                                sum += hitsPayloadLengths[hitsIndex++];

                        it->payloadsIt += sum;
                        it->hitsIndex = hitsIndex;
#endif

                        it->skippedHits -= step;
                        rem -= step;
                } while (rem);
        }
}

void Trinity::Codecs::Lucene::Decoder::refill_documents(Trinity::Codecs::Lucene::PostingsListIterator *it)
{
        if (it->docsLeft >= BLOCK_SIZE)
        {
#ifdef LUCENE_USE_FASTPFOR
                it->p = ints_decode(forUtil, it->p, it->docDeltas);
                it->p = ints_decode(forUtil, it->p, it->docFreqs);
#else
                it->p = ints_decode(it->p, it->docDeltas);
                it->p = ints_decode(it->p, it->docFreqs);
#endif

                it->bufferedDocs = BLOCK_SIZE;
                it->docsLeft -= BLOCK_SIZE;
        }
        else
        {
                uint32_t v;
                auto p{it->p};
                auto &docFreqs{it->docFreqs};
                auto &docDeltas{it->docDeltas};
                const auto docsLeft{it->docsLeft};

                for (uint32_t i{0}; i != docsLeft; ++i)
                {
                        varbyte_get32(p, v);

#if defined(LUCENE_ENCODE_FREQ1_DOCDELTA)
                        docDeltas[i] = v >> 1;
                        if (v & 1)
                                docFreqs[i] = 1;
                        else
                        {
                                varbyte_get32(p, v);
                                docFreqs[i] = v;
                        }
#else
                        docDeltas[i] = v;
                        varbyte_get32(p, v);
                        docFreqs[i] = v;
#endif
                }
                it->p = p; // restore
                it->bufferedDocs = docsLeft;
                it->docsLeft = 0;
        }

        it->docsIndex = 0;
        update_curdoc(it);
}

void Trinity::Codecs::Lucene::Decoder::decode_next_block(Trinity::Codecs::Lucene::PostingsListIterator *it)
{
        // this is important
        if (const auto n = it->skippedHits)
                skip_hits(it, n);

        refill_documents(it);
}

[[gnu::hot]] void Trinity::Codecs::Lucene::Decoder::next(Trinity::Codecs::Lucene::PostingsListIterator *const __restrict__ it)
{
        auto &docFreqs{it->docFreqs};
        auto &docDeltas{it->docDeltas};
        auto idx{it->docsIndex};

        it->skippedHits += docFreqs[idx];
        it->lastDocID += docDeltas[idx++];

        if (unlikely(idx >= it->bufferedDocs))
        {
                if (likely(it->p != chunkEnd))
                {
                        it->docsIndex = idx;

                        decode_next_block(it);

                        idx = it->docsIndex;
                }
                else
                {
                        finalize(it);

                        it->docsIndex = idx;
                        return;
                }
        }

        it->curDocument.id = it->lastDocID + docDeltas[idx];
        it->freq = docFreqs[idx];
        it->docsIndex = idx;
}

uint32_t Trinity::Codecs::Lucene::Decoder::skiplist_search(PostingsListIterator *it, const isrc_docid_t target) const noexcept
{
#if 0
        uint32_t idx{UINT32_MAX};

        for (int32_t top{int32_t(skiplist.size) - 1}, btm{int32_t(it->skipListIdx)}; btm <= top;)
        {
                const auto mid = (btm + top) / 2;
                const auto v = skiplist.data[mid].lastDocID;

                if (target < v)
                        top = mid - 1;
                else
                {
                        if (v != target)
                                idx = mid;
                        else if (mid != it->skipListIdx)
                        {
                                // we need this
                                idx = mid - 1;
                        }

                        btm = mid + 1;
                }
        }

        return idx;
#else
        // branchless binary search
        // See: http://databasearchitects.blogspot.gr/2015/09/trying-to-speed-up-binary-search.html
        // Need to verify this, but looks fine so far
        //
        // This compiles down to (modulo loading instructions for it->skipListIdx)
        /*
 	 *
	.L4:
	  movl %esi, %edi
	  leaq (%rcx,%rdi,4), %rdi
	  cmpl (%rdi), %r8d
	  cmova %rdi, %rcx
	  subl %esi, %edx
	  movl %edx, %esi
	  shrl %esi
	  jne .L4
	*
	*/
        // which is pretty good - no branches, and few instructions
        const auto idx{it->skipListIdx};
        const auto *data = skiplist.data + idx;
        uint32_t n = skiplist.size - idx;

        while (const auto h = n / 2)
        {
                const auto m = data + h;

                data = (m->lastDocID < target) ? m : data;
                n -= h;
        }

        return target > data->lastDocID ? data - skiplist.data : UINT32_MAX;
#endif
}

[[gnu::hot]] void Trinity::Codecs::Lucene::Decoder::advance(Trinity::Codecs::Lucene::PostingsListIterator *it, const isrc_docid_t target)
{
        auto localBufferedDocs{it->bufferedDocs}; // the compiler may be able to better deal with aliasing here

#ifdef LUCENE_LAZY_SKIPLIST_INIT
        if (unlikely(skiplistSize))
        {
                init_skiplist(skiplistSize);
                skiplistSize = 0;
        }
#endif

        auto &curDocument{it->curDocument};
        auto &docFreqs{it->docFreqs};
        auto &docDeltas{it->docDeltas};
        auto docsIndex{it->docsIndex};

#ifdef LUCENE_SKIPLIST_SEEK_EARLY
        if (target > it->curSkipListLastDocID)
        {
                // We need to skip ahead right now
                goto skip1;
        }
#endif

        for (;;)
        {
                if (unlikely(docsIndex == localBufferedDocs))
                {
                        if (unlikely(it->p == chunkEnd))
                        {
                                finalize(it);
                                it->docsIndex = docsIndex;
                                return;
                        }
                        else
                        {
#if 1
                                if (it->skipListIdx != skiplist.size)
                                {
                                // see if we can determine where to seek to here
                                skip1:
                                        if (const auto index = skiplist_search(it, target); index != UINT32_MAX)
                                        {
                                                // we can advance here; we will only attempt to skiplist search
                                                // next time we are done with a block
                                                it->skipListIdx = index + 1;
#ifdef LUCENE_SKIPLIST_SEEK_EARLY
                                                if (SKIPLIST_STEP == 1)
                                                        it->curSkipListLastDocID = it->skipListIdx == skiplist.size ? DocIDsEND : skiplist.data[it->skipListIdx].lastDocID;
#endif

                                                const auto &r = skiplist.data[index];

#if 1
                                                const auto blockPtr = postingListBase + r.indexOffset;
                                                const auto hitsBlockPtr = hitsBase + r.lastHitsBlockOffset;

                                                it->p = blockPtr;
                                                it->hdp = hitsBlockPtr;

                                                it->lastDocID = r.lastDocID;
                                                it->docsLeft = totalDocuments - r.totalDocumentsSoFar;
                                                it->hitsLeft = totalHits - r.totalHitsSoFar;

                                                it->skippedHits = 0;
                                                it->bufferedHits = 0;

                                                refill_documents(it);
                                                refill_hits(it);
                                                update_curdoc(it);

                                                it->skippedHits = r.curHitsBlockHits;
                                                if (const auto n = it->skippedHits)
                                                        skip_hits(it, n);

                                                localBufferedDocs = it->bufferedDocs;
                                                docsIndex = it->docsIndex;
                                                goto l10;
#endif
                                        }
                                }
#endif

                                if (trace)
                                        SLog("Will decode next block\n");

                                decode_next_block(it);
                                localBufferedDocs = it->bufferedDocs;
                                docsIndex = it->docsIndex;
                        }
                }
                else
                {
                l10:
                        if (curDocument.id == target)
                        {
                                if (trace)
                                        SLog("Found it\n");

                                it->docsIndex = docsIndex;
                                return;
                        }
                        else if (curDocument.id > target)
                        {
                                if (trace)
                                        SLog("Not Here, now past target\n");

                                it->docsIndex = docsIndex;
                                return;
                        }
                        else
                        {
                                it->skippedHits += docFreqs[docsIndex];
                                it->lastDocID += docDeltas[docsIndex];

                                ++docsIndex;

                                // see: update_curdoc();
                                curDocument.id = it->lastDocID + docDeltas[docsIndex];
                                it->freq = docFreqs[docsIndex];
                        }
                }
        }
}

void Trinity::Codecs::Lucene::Decoder::materialize_hits(PostingsListIterator *it, DocWordsSpace *const __restrict__ dws, term_hit *const __restrict__ out)
{
        const auto termID{execCtxTermID};
        auto freq = it->docFreqs[it->docsIndex];
        auto outPtr = out;

        if (const auto skippedHits = it->skippedHits)
                skip_hits(it, skippedHits);

        // fast-path; can satisfy directly from the current hits block
        auto hitsIndex = it->hitsIndex;
        auto &hitsPayloadLengths{it->hitsPayloadLengths};
        auto &hitsPositionDeltas{it->hitsPositionDeltas};
        const auto bufferedHits{it->bufferedHits};


        if (const auto upto = hitsIndex + freq; upto <= bufferedHits)
        {
                tokenpos_t pos{0};

                while (hitsIndex != upto)
                {
                        const auto pl = hitsPayloadLengths[hitsIndex];

                        pos += hitsPositionDeltas[hitsIndex];
                        outPtr->pos = pos;
                        outPtr->payloadLen = pl;


                        if (pos)
                                dws->set(termID, pos);

			outPtr->payload = 0;
                        if (pl)
                        {
                                memcpy(&outPtr->payload, it->payloadsIt, pl);
                                it->payloadsIt += pl;
                        }

                        ++outPtr;
                        ++hitsIndex;
                }
        }
        else
        {
                tokenpos_t pos{0};

                for (;;)
                {
                        const auto n = std::min<uint32_t>(it->bufferedHits - hitsIndex, freq);
                        const auto upto = hitsIndex + n;


                        while (hitsIndex != upto)
                        {
                                const auto pl = hitsPayloadLengths[hitsIndex];

                                pos += hitsPositionDeltas[hitsIndex];
                                outPtr->pos = pos;
                                outPtr->payloadLen = pl;

                                if (pos)
                                        dws->set(termID, pos);

				outPtr->payload = 0;
				if (pl)
				{
					memcpy(&outPtr->payload, it->payloadsIt, pl);
					it->payloadsIt += pl;
				}

                                ++outPtr;
                                ++hitsIndex;
                        }
                        freq -= n;

                        if (freq)
                        {
                                it->hitsIndex = hitsIndex;
                                refill_hits(it);
                                hitsIndex = it->hitsIndex;
                        }
                        else
                                break;
                }
        }

        it->hitsIndex = hitsIndex;       // restore
        it->docFreqs[it->docsIndex] = 0; // simplifies processing logic
}

Trinity::Codecs::PostingsListIterator *Trinity::Codecs::Lucene::Decoder::new_iterator()
{
        auto it = std::make_unique<Trinity::Codecs::Lucene::PostingsListIterator>(this);

        it->lastDocID = 0;
        it->lastPosition = 0;
        it->docsLeft = totalDocuments;
        it->hitsLeft = totalHits;
        it->docsIndex = it->hitsIndex = 0;
        it->bufferedDocs = it->bufferedHits = 0;
        it->skippedHits = 0;
        it->docFreqs[0] = 0;
        it->docDeltas[0] = 0;
        it->skipListIdx = 0;
        it->hdp = hitsBase;
        it->p = postingListBase + sizeof(uint32_t) + sizeof(uint32_t) + sizeof(uint32_t) + sizeof(uint16_t);

        return it.release();
}

void Trinity::Codecs::Lucene::Decoder::init_skiplist(const uint16_t size)
{
        static constexpr size_t skiplistEntrySize{sizeof(uint32_t) * 5 + sizeof(uint16_t)};
        const auto *sit = chunkEnd;

        skiplist.size = size;
        skiplist.data = (skiplist_entry *)malloc(sizeof(skiplist_entry) * size);
        for (uint32_t i{0}; i != size; ++i, sit += skiplistEntrySize)
        {
                const auto it = reinterpret_cast<const uint32_t *>(sit);
                auto &e = skiplist.data[i];

                e.indexOffset = it[0];
                e.lastDocID = it[1];
                e.lastHitsBlockOffset = it[2];
                e.totalDocumentsSoFar = it[3];
                e.totalHitsSoFar = it[4];
                e.curHitsBlockHits = *(uint16_t *)(sit + skiplistEntrySize - sizeof(uint16_t));
        }
}

void Trinity::Codecs::Lucene::Decoder::init(const term_index_ctx &tctx, Trinity::Codecs::AccessProxy *access)
{
        auto ap = static_cast<Trinity::Codecs::Lucene::AccessProxy *>(access);
        const auto indexPtr = ap->indexPtr;
        const auto ptr = indexPtr + tctx.indexChunk.offset;
        const auto chunkSize = tctx.indexChunk.size();
        auto p = ptr;

        indexTermCtx = tctx;
        postingListBase = ptr;
        chunkEnd = ptr + chunkSize;
        totalDocuments = tctx.documents;

        const auto hitsDataOffset = *(uint32_t *)p;
        p += sizeof(uint32_t);
        totalHits = *(uint32_t *)p;
        p += sizeof(uint32_t);
        p += sizeof(uint32_t); // positions chunk size
#ifdef LUCENE_LAZY_SKIPLIST_INIT
        skiplistSize = *(uint16_t *)p;
#else
        const auto skiplistSize = *(uint16_t *)p;
#endif
        p += sizeof(uint16_t);

        if (skiplistSize)
        {
                // deserialize the skiplist and maybe use it
                static constexpr size_t skiplistEntrySize{sizeof(uint32_t) * 5 + sizeof(uint16_t)};

                chunkEnd = (ptr + chunkSize) - (skiplistSize * skiplistEntrySize);

#ifndef LUCENE_LAZY_SKIPLIST_INIT
                init_skiplist(skiplistSize);
#endif
        }

        hitsBase = ap->hitsDataPtr + hitsDataOffset;
}

Trinity::Codecs::Lucene::AccessProxy::~AccessProxy()
{
	if (hitsDataSize)
	{
		// mmmaped()/owned by this AccessProxy
		munmap((void *)hitsDataPtr, hitsDataSize);
	}
}

Trinity::Codecs::Lucene::AccessProxy::AccessProxy(const char *bp, const uint8_t *p, const uint8_t *hd)
    : Trinity::Codecs::AccessProxy{bp, p}, hitsDataPtr{hd}
{
        if (hd == nullptr)
        {
                int fd = open(Buffer{}.append(basePath, "/hits.data").c_str(), O_RDONLY | O_LARGEFILE);

                if (fd == -1)
                {
                        if (errno != ENOENT)
                                throw Switch::data_error("Unable to access hits.data");
                }
                else if (const auto fileSize = lseek64(fd, 0, SEEK_END); fileSize > 0)
                {
                        hitsDataPtr = reinterpret_cast<const uint8_t *>(mmap(nullptr, fileSize, PROT_READ, MAP_SHARED, fd, 0));

                        close(fd);
                        expect(hitsDataPtr != MAP_FAILED);
			madvise((void *)hitsDataPtr, fileSize, MADV_DONTDUMP);
			hitsDataSize = fileSize;
                }
                else
		{
                        close(fd);
		}
        }
}

void Trinity::Codecs::Lucene::IndexSession::merge(merge_participant *const __restrict__ participants, const uint16_t participantsCnt, Trinity::Codecs::Encoder *const __restrict__ enc_)
{
        // This is somewhat complicated
        static constexpr bool trace{false};

        struct candidate
        {
                isrc_docid_t lastDocID;
                isrc_docid_t docDeltas[BLOCK_SIZE];
                uint32_t docFreqs[BLOCK_SIZE];
                uint32_t hitsPayloadLengths[BLOCK_SIZE];
                uint32_t hitsPositionDeltas[BLOCK_SIZE];
                masked_documents_registry *maskedDocsReg;

                uint32_t documentsLeft;
                uint32_t hitsLeft;
                uint32_t skippedHits;
                uint16_t bufferedHits;
                uint16_t hitsIndex;

                struct
                {
                        const uint8_t *p;
                        const uint8_t *e;
                } index_chunk;

                struct
                {
                        const uint8_t *p;
                        const uint8_t *e;
                } positions_chunk;

                struct
                {
                        uint8_t i;
                        uint8_t size;
                } cur_block;

                const uint8_t *payloadsIt, *payloadsEnd;

#ifdef LUCENE_USE_FASTPFOR
                void refill_hits(FastPForLib::FastPFor<4> &forUtil)
#else
                void refill_hits()
#endif
                {
                        uint32_t payloadsChunkLength;
                        auto hdp = positions_chunk.p;

                        if (trace)
                                SLog(ansifmt::bold, "REFILLING NOW, hitsLeft = ", hitsLeft, ", hitsIndex = ", hitsIndex, ", bufferedHits = ", bufferedHits, ansifmt::reset, "\n");

                        require(hitsIndex == 0 || hitsIndex == BLOCK_SIZE);

                        if (hitsLeft >= BLOCK_SIZE)
                        {
#ifdef LUCENE_USE_FASTPFOR
                                hdp = ints_decode(forUtil, hdp, hitsPositionDeltas);
                                hdp = ints_decode(forUtil, hdp, hitsPayloadLengths);
#else
                                hdp = ints_decode(hdp, hitsPositionDeltas);
                                hdp = ints_decode(hdp, hitsPayloadLengths);
#endif

                                varbyte_get32(hdp, payloadsChunkLength);

                                payloadsIt = hdp;
                                hdp += payloadsChunkLength;
                                payloadsEnd = hdp;

                                bufferedHits = BLOCK_SIZE;
                                hitsLeft -= BLOCK_SIZE;
                        }
                        else
                        {
                                uint32_t v;
                                uint8_t payloadLen{0};

                                payloadsChunkLength = 0;
                                for (uint32_t i{0}; i != hitsLeft; ++i)
                                {
                                        varbyte_get32(hdp, v);

                                        if (v & 1)
                                                payloadLen = *hdp++;

                                        hitsPositionDeltas[i] = v >> 1;
                                        hitsPayloadLengths[i] = payloadLen;
                                        payloadsChunkLength += payloadLen;
                                }

                                payloadsIt = hdp;
                                hdp += payloadsChunkLength;
                                payloadsEnd = hdp;

                                bufferedHits = hitsLeft;
                                hitsLeft = 0;
                        }

                        positions_chunk.p = hdp;
                        hitsIndex = 0;
                }

#ifdef LUCENE_USE_FASTPFOR
                void refill_documents(FastPForLib::FastPFor<4> &forUtil)
#else
                void refill_documents()
#endif
                {
                        if (trace)
                                SLog("Refilling documents ", documentsLeft, "\n");

                        if (documentsLeft >= BLOCK_SIZE)
                        {
#ifdef LUCENE_USE_FASTPFOR
                                index_chunk.p = ints_decode(forUtil, index_chunk.p, docDeltas);
                                index_chunk.p = ints_decode(forUtil, index_chunk.p, docFreqs);
#else
                                index_chunk.p = ints_decode(index_chunk.p, docDeltas);
                                index_chunk.p = ints_decode(index_chunk.p, docFreqs);
#endif

                                cur_block.size = BLOCK_SIZE;
                                documentsLeft -= BLOCK_SIZE;
                        }
                        else
                        {
                                uint32_t v;
                                auto p = index_chunk.p;

                                for (uint32_t i{0}; i != documentsLeft; ++i)
                                {
                                        varbyte_get32(p, v);

#if defined(LUCENE_ENCODE_FREQ1_DOCDELTA)
                                        docDeltas[i] = v >> 1;
                                        if (v & 1)
                                                docFreqs[i] = 1;
                                        else
                                        {
                                                varbyte_get32(p, v);
                                                docFreqs[i] = v;
                                        }
#else
                                        docDeltas[i] = v;
                                        varbyte_get32(p, v);
                                        docFreqs[i] = v;
#endif
                                }
                                index_chunk.p = p;

                                cur_block.size = documentsLeft;
                                documentsLeft = 0;
                        }
                        cur_block.i = 0;

                        if (trace)
                                SLog(cur_block.i, " ", cur_block.size, "\n");
                }

#ifdef LUCENE_USE_FASTPFOR
                void skip_ommitted_hits(FastPForLib::FastPFor<4> &forUtil)
#else
                void skip_ommitted_hits()
#endif
                {
                        if (trace)
                                SLog("Skipping omitted hits ", skippedHits, ", bufferedHits = ", bufferedHits, "\n");

                        if (!skippedHits)
                        {
                                // There was a silly fast-path optimization here
                                //
                                // if (!skipppedHits) return;
                                // else if (bufferedHits == skippedHits){ skippedHits = 0; bufferedHits = 0; hitsIndex = 0; payloadsIt = payloadsEnd; return;}
                                // else { ... }
                                //
                                // which was causing all kinds of random problems(missing positions/payloads etc)
                                // I should be able to figure out exactly what's wrong here, but just dropping this fixed everything and it's also simpler to reason about the state anyway. Better off without it
                                return;
                        }
                        else
                        {
                                do
                                {
                                        if (hitsIndex == bufferedHits)
                                        {
#ifdef LUCENE_USE_FASTPFOR
                                                refill_hits(forUtil);
#else
                                                refill_hits();
#endif
                                        }

                                        const auto step = std::min<uint32_t>(skippedHits, bufferedHits - hitsIndex);

                                        for (uint32_t i{0}; i != step; ++i)
                                        {
                                                const auto pl = hitsPayloadLengths[hitsIndex++];

                                                payloadsIt += pl;
                                        }

                                        skippedHits -= step;
                                } while (skippedHits);
                        }
                }

#ifdef LUCENE_USE_FASTPFOR
                void output_hits(FastPForLib::FastPFor<4> &forUtil, Trinity::Codecs::Lucene::Encoder *__restrict__ enc)
#else
                void output_hits(Trinity::Codecs::Lucene::Encoder *__restrict__ enc)
#endif
                {
                        auto freq = docFreqs[cur_block.i];
                        uint64_t payload;
                        tokenpos_t pos{0};

                        if (trace)
                                SLog("Will output hits for ", cur_block.i, " ", freq, ", skippedHits = ", skippedHits, "\n");

#ifdef LUCENE_USE_FASTPFOR
                        skip_ommitted_hits(forUtil);
#else
                        skip_ommitted_hits();
#endif

                        if (const auto upto = hitsIndex + freq; upto <= bufferedHits)
                        {
                                if (trace)
                                        SLog("fast-path hitsIndex = ", hitsIndex, ", upto = ", upto, "\n");

                                while (hitsIndex != upto)
                                {
                                        pos += hitsPositionDeltas[hitsIndex];

                                        const auto pl = hitsPayloadLengths[hitsIndex];

					payload = 0;
                                        if (pl)
                                        {
                                                memcpy(&payload, payloadsIt, pl);
                                                payloadsIt += pl;
                                        }

                                        enc->new_hit(pos, {(uint8_t *)&payload, uint8_t(pl)});

                                        ++hitsIndex;
                                }
                        }
                        else
                        {
                                if (trace)
                                        SLog("SLOW path\n");

                                for (;;)
                                {
                                        const auto n = std::min<uint32_t>(bufferedHits - hitsIndex, freq);
                                        const auto upto = hitsIndex + n;

                                        if (trace)
                                                SLog("upto = ", upto, ", bufferedHits = ", bufferedHits, ", hitsIndex = ", hitsIndex, "\n");

                                        while (hitsIndex != upto)
                                        {
                                                pos += hitsPositionDeltas[hitsIndex];

                                                const auto pl = hitsPayloadLengths[hitsIndex];

						payload = 0;
                                                if (pl)
                                                {
                                                        memcpy(&payload, payloadsIt, pl);
                                                        payloadsIt += pl;
                                                }

                                                enc->new_hit(pos, {(uint8_t *)&payload, uint8_t(pl)});

                                                ++hitsIndex;
                                        }

                                        freq -= n;

                                        if (freq)
                                        {
                                                if (trace)
                                                        SLog("Will refill hits (Freq now = ", freq, ")\n");

#ifdef LUCENE_USE_FASTPFOR
                                                refill_hits(forUtil);
#else
                                                refill_hits();
#endif
                                        }
                                        else
                                                break;
                                }
                        }

                        docFreqs[cur_block.i] = 0; // simplifies processing logic (See next().)
                }

#ifdef LUCENE_USE_FASTPFOR
                bool next(FastPForLib::FastPFor<4> &forUtil)
#else
                bool next()
#endif
                {
                        skippedHits += docFreqs[cur_block.i];
                        lastDocID += docDeltas[cur_block.i++];

                        if (cur_block.i == cur_block.size)
                        {
                                if (trace)
                                        SLog("End of block, documentsLeft = ", documentsLeft, "\n");

                                if (!documentsLeft)
                                        return false;

// this is important, because refill_documents()
// will update cur_block
#ifdef LUCENE_USE_FASTPFOR
                                skip_ommitted_hits(forUtil);
#else
                                skip_ommitted_hits();
#endif

#ifdef LUCENE_USE_FASTPFOR
                                refill_documents(forUtil);
#else
                                refill_documents();
#endif
                        }
                        else
                        {
                                if (trace)
                                        SLog("NOW at ", cur_block.i, "\n");
                        }

                        return true;
                }

                constexpr auto current() noexcept
                {
                        return lastDocID + docDeltas[cur_block.i];
                }

                constexpr auto current_freq() noexcept
                {
                        return docFreqs[cur_block.i];
                }
        };

        candidate candidates[participantsCnt];
        uint16_t rem{participantsCnt};
        uint16_t toAdvance[participantsCnt];
        auto encoder = static_cast<Trinity::Codecs::Lucene::Encoder *>(enc_);

        for (uint32_t i{0}; i != participantsCnt; ++i)
        {
                auto c = candidates + i;
                const auto ap = static_cast<const Trinity::Codecs::Lucene::AccessProxy *>(participants[i].ap);
                const auto *p = ap->indexPtr + participants[i].tctx.indexChunk.offset;

                c->index_chunk.e = p + participants[i].tctx.indexChunk.size();
                c->maskedDocsReg = participants[i].maskedDocsReg;
                c->documentsLeft = participants[i].tctx.documents;
                c->lastDocID = 0;
                c->skippedHits = 0;
                c->hitsIndex = 0;
                c->bufferedHits = 0;
                c->payloadsIt = c->payloadsEnd = nullptr;

                const auto hitsDataOffset = *(uint32_t *)p;
                p += sizeof(uint32_t);
                const auto sumHits = *(uint32_t *)p;
                p += sizeof(uint32_t);
                const auto posChunkSize = *(uint32_t *)p;
                p += sizeof(uint32_t);
                [[maybe_unused]] const auto skiplistSize = *(uint16_t *)p;
                p += sizeof(uint16_t);

                c->index_chunk.p = p;
                c->positions_chunk.p = ap->hitsDataPtr + hitsDataOffset;
                c->positions_chunk.e = c->positions_chunk.p + posChunkSize;
                c->hitsLeft = sumHits;

                if (trace)
                        SLog("participant ", i, " ", c->documentsLeft, " ", c->hitsLeft, ", skiplistSize = ", skiplistSize, "\n");

                // Skip past skiplist
                if (skiplistSize)
                {
                        static constexpr size_t skiplistEntrySize{sizeof(uint32_t) * 5 + sizeof(uint16_t)};

                        c->index_chunk.e -= skiplistSize * skiplistEntrySize;
                }

#ifdef LUCENE_USE_FASTPFOR
                c->refill_documents(forUtil);
#else
                c->refill_documents();
#endif
        }

        for (isrc_docid_t prev{0};;)
        {
                uint16_t toAdvanceCnt{1};
                auto did = candidates[0].current();

                toAdvance[0] = 0;
                for (uint32_t i{1}; i != rem; ++i)
                {
                        if (const auto id = candidates[i].current(); id == did)
                                toAdvance[toAdvanceCnt++] = i;
                        else if (id < did)
                        {
                                did = id;
                                toAdvanceCnt = 1;
                                toAdvance[0] = i;
                        }
                }

                require(did > prev);
                prev = did;

                const auto c = candidates + toAdvance[0]; // always choose the first because they are sorted in-order

                if (!c->maskedDocsReg->test(did))
                {
                        [[maybe_unused]] const auto freq = c->current_freq();

                        encoder->begin_document(did);
#ifdef LUCENE_USE_FASTPFOR
                        c->output_hits(forUtil, encoder);
#else
                        c->output_hits(encoder);
#endif
                        encoder->end_document();
                }

                do
                {
                        const auto idx = toAdvance[--toAdvanceCnt];
                        auto c = candidates + idx;

#ifdef LUCENE_USE_FASTPFOR
                        if (!c->next(forUtil))
#else
                        if (!c->next())
#endif
                        {
                                if (!--rem)
                                        goto l1;

                                memmove(candidates + idx, candidates + idx + 1, (rem - idx) * sizeof(candidates[0]));
                        }

                } while (toAdvanceCnt);
        }

l1:;
}

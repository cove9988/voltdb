/* This file is part of VoltDB.
 * Copyright (C) 2008-2011 VoltDB Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#include <cstring>
#include <cstdlib>
#include <queue>
#include <deque>
#include "harness.h"
#include "common/types.h"
#include "common/NValue.hpp"
#include "common/ValueFactory.hpp"
#include "common/TupleSchema.h"
#include "common/tabletuple.h"
#include "storage/StreamBlock.h"
#include "storage/TupleStreamWrapper.h"
#include "common/Topend.h"
#include "common/executorcontext.hpp"
#include "boost/smart_ptr.hpp"

using namespace std;
using namespace voltdb;
using namespace boost;

const int COLUMN_COUNT = 5;
// Annoyingly, there's no easy way to compute the exact Exported tuple
// size without incestuously using code we're trying to test.  I've
// pre-computed this magic size for an Exported tuple of 5 integer
// columns, which includes:
// 5 Export header columns * sizeof(int64_t) = 40
// 1 Export header column * sizeof(int64_t) = 8
// 2 bytes for null mask (10 columns rounds to 16, /8 = 2) = 2
// sizeof(int32_t) for row header = 4
// 5 * sizeof(int64_t) for tuple data = 40
// total: 94
const int MAGIC_TUPLE_SIZE = 94;
// 1k buffer
const int BUFFER_SIZE = 1024;

class DummyTopend : public Topend {
public:
    DummyTopend() : receivedExportBuffer(false),
                    receivedEndOfStream(false)
    {

    }

    int loadNextDependency(
        int32_t dependencyId, voltdb::Pool *pool, Table* destination) {
        return 0;
    }

    void crashVoltDB(voltdb::FatalException e) {
    }

    int64_t getQueuedExportBytes(int32_t partitionId, string signature) {
        int64_t bytes = 0;
        for (int ii = 0; ii < blocks.size(); ii++) {
            bytes += blocks[ii]->rawLength();
        }
        return bytes;
    }

    virtual void pushExportBuffer(int64_t generation, int32_t partitionId,
                                  string signature,
                                  vector<const string*> columnNames,
                                  StreamBlock *block,
                                  bool sync, bool endOfStream)
    {
        partitionIds.push(partitionId);
        signatures.push(signature);
        if (block != NULL)
        {
            blocks.push_back(shared_ptr<StreamBlock>(new StreamBlock(block)));
            data.push_back(shared_ptr<char>(block->rawPtr()));
        }
        receivedExportBuffer = true;
        if (!receivedEndOfStream)
        {
            receivedEndOfStream = endOfStream;
        }
        for (int i = 0; i < columnNames.size(); i++)
        {
            m_columnNames.push_back(columnNames[i]);
        }
    }

    void fallbackToEEAllocatedBuffer(char *buffer, size_t length) {}
    queue<int32_t> partitionIds;
    queue<string> signatures;
    deque<shared_ptr<StreamBlock> > blocks;
    vector<shared_ptr<char> > data;
    vector<const string*> m_columnNames;
    bool receivedExportBuffer;
    bool receivedEndOfStream;
};

class TupleStreamWrapperTest : public Test {
public:
    TupleStreamWrapperTest() : m_wrapper(NULL), m_schema(NULL), m_tuple(NULL),
                               m_context(new ExecutorContext( 1, 1, NULL, &m_topend, true, 0, 60000, "localhost", 2))
    {
        srand(0);

        // set up the schema used to fill the new buffer
        vector<ValueType> columnTypes;
        vector<int32_t> columnLengths;
        vector<bool> columnAllowNull;
        m_columnNames[0] = "COLUMN0";
        m_columnNames[1] = "COLUMN1";
        m_columnNames[2] = "COLUMN2";
        m_columnNames[3] = "COLUMN3";
        m_columnNames[4] = "COLUMN4";
        for (int i = 0; i < COLUMN_COUNT; i++) {
            columnTypes.push_back(VALUE_TYPE_INTEGER);
            columnLengths.push_back(NValue::getTupleStorageSize(VALUE_TYPE_INTEGER));
            columnAllowNull.push_back(false);
        }
        m_schema =
          TupleSchema::createTupleSchema(columnTypes,
                                         columnLengths,
                                         columnAllowNull,
                                         true);

        // allocate a new buffer and wrap it
        m_wrapper = new TupleStreamWrapper(1, 1, COLUMN_COUNT, m_columnNames);

        // excercise a smaller buffer capacity
        m_wrapper->setDefaultCapacity(BUFFER_SIZE);

        // Set the initial generation (pretend to do the first catalog load)
        m_wrapper->setSignatureAndGeneration("dude", 0);

        // set up the tuple we're going to use to fill the buffer
        // set the tuple's memory to zero
        ::memset(m_tupleMemory, 0, 8 * (COLUMN_COUNT + 1));

        // deal with the horrible hack that needs to set the first
        // value to true (rtb?? what is this horrible hack?)
        *(reinterpret_cast<bool*>(m_tupleMemory)) = true;
        m_tuple = new TableTuple(m_schema);
        m_tuple->move(m_tupleMemory);
    }

    void appendTuple(int64_t lastCommittedTxnId, int64_t currentTxnId,
                     int64_t generationId)
    {
        // fill a tuple
        for (int col = 0; col < COLUMN_COUNT; col++) {
            int value = rand();
            m_tuple->setNValue(col, ValueFactory::getIntegerValue(value));
        }
        // append into the buffer
        m_wrapper->appendTuple(lastCommittedTxnId,
                               currentTxnId, 1, 1, generationId,
                               *m_tuple,
                               TupleStreamWrapper::INSERT);
    }

    virtual ~TupleStreamWrapperTest() {
        delete m_wrapper;
        delete m_tuple;
        if (m_schema)
            TupleSchema::freeTupleSchema(m_schema);
    }

protected:
    TupleStreamWrapper* m_wrapper;
    TupleSchema* m_schema;
    char m_tupleMemory[(COLUMN_COUNT + 1) * 8];
    TableTuple* m_tuple;
    DummyTopend m_topend;
    scoped_ptr<ExecutorContext> m_context;
    string m_columnNames[COLUMN_COUNT];
};

// Several of these cases were move to TestExportDataSource in Java
// where some TupleStreamWrapper functionality now lives
// Cases of interest:
// 1. periodicFlush with a clean buffer (no open txns) generates a new buffer
//    DONE
// 2. appendTuple fills and generates a new buffer (committed TXN ID advances)
//    DONE
// 3. appendTuple fills a buffer with a single TXN ID, uncommitted,
//    commits somewhere in the next buffer
//    DONE
// 4. case 3 but where commit is via periodic flush
//    DONE
// 5. case 2 but where the last tuple is rolled back
//    DONE
// 6. periodicFlush with a busy buffer (an open txn) doesn't generate a new buffer
//    DONE
// 7. roll back the last tuple, periodicFlush, get the expected length
//    DONE
// 8. Case 1 but where the first buffer is just released, not polled
//    DONE
// 9. Roll back a transaction that has filled more than one buffer,
//    then add a transaction, then commit and poll
//    DONE
// 10. Rollback the first tuple, then append, make sure only 1 tuple
//     DONE
// 11. Test that releasing tuples that aren't committed returns an error
//     DONE
// 12. Test that a release value that isn't a buffer boundary returns an error
//     DONE
// 13. Test that releasing all the data followed by a poll results in no data
//     DONE
// 14. Test that a periodicFlush with both txn IDs far in the future behaves
//     correctly
//     DONE
// 15. Test that a release value earlier than our current history return safely
//     DONE
// 16. Test that a release that includes all the pending buffers works properly
//     DONE
//---
// Additional floating release/poll tests
//
// 17. Test that a release in the middle of a finished buffer followed
//     by a poll returns a StreamBlock with a proper releaseOffset
//     (and other meta-data), basically consistent with handing the
//     un-ack'd portion of the block to Java.
//     - Invalidates old test (12)
//
// 18. Test that a release in the middle of the current buffer returns
//     a StreamBlock consistent with indicating that no data is
//     currently available.  Then, if that buffer gets filled and
//     finished, that the next poll returns the correct remainder of
//     that buffer.
// ---
// New test for export refactoring 9/27/11
//
// Test that advancing the generation ID value results in a new
// StreamBlock boundary.
//
// Test that rolling back a tuple in a fresh StreamBlock and then
// appending a tuple that advances the generation ID results in a new
// StreamBlock boundary

/**
 * Get one tuple
 */
TEST_F(TupleStreamWrapperTest, DoOneTuple)
{

    // write a new tuple and then flush the buffer
    appendTuple(1, 2, 0);
    m_wrapper->periodicFlush(-1, 2, 2);

    // we should only have one tuple in the buffer
    ASSERT_TRUE(m_topend.receivedExportBuffer);
    shared_ptr<StreamBlock> results = m_topend.blocks.front();
    EXPECT_EQ(results->uso(), 0);
    EXPECT_EQ(results->generationId(), 0);
    EXPECT_EQ(results->offset(), MAGIC_TUPLE_SIZE);
    // Check the column names for good measure
    EXPECT_EQ(m_topend.m_columnNames.size(), COLUMN_COUNT);
    for (int i = 0; i < m_topend.m_columnNames.size(); i++)
    {
        EXPECT_EQ(*m_topend.m_columnNames[i], m_columnNames[i]);
    }
}

/**
 * Test the really basic operation order
 */
TEST_F(TupleStreamWrapperTest, BasicOps)
{

    // verify the block count statistic.
    size_t allocatedByteCount = m_wrapper->allocatedByteCount();
    ASSERT_TRUE(allocatedByteCount == 0);

    for (int i = 1; i < 10; i++)
    {
        appendTuple(i-1, i, 0);
    }
    m_wrapper->periodicFlush(-1, 9, 10);

    for (int i = 10; i < 20; i++)
    {
        appendTuple(i-1, i, 0);
    }
    m_wrapper->periodicFlush(-1, 19, 19);

    EXPECT_EQ( 1786, m_wrapper->allocatedByteCount());

    // get the first buffer flushed
    ASSERT_TRUE(m_topend.receivedExportBuffer);
    shared_ptr<StreamBlock> results = m_topend.blocks.front();
    m_topend.blocks.pop_front();
    EXPECT_EQ(results->uso(), 0);
    EXPECT_EQ(results->generationId(), 0);
    EXPECT_EQ(results->offset(), (MAGIC_TUPLE_SIZE * 9));

    // now get the second
    ASSERT_FALSE(m_topend.blocks.empty());
    results = m_topend.blocks.front();
    m_topend.blocks.pop_front();
    EXPECT_EQ(results->uso(), (MAGIC_TUPLE_SIZE * 9));
    EXPECT_EQ(results->generationId(), 0);
    EXPECT_EQ(results->offset(), (MAGIC_TUPLE_SIZE * 10));

    // ack all of the data and re-verify block count
    allocatedByteCount = m_wrapper->allocatedByteCount();
    ASSERT_TRUE(allocatedByteCount == 0);
}

/**
 * Verify that a periodicFlush with distant TXN IDs works properly
 */
TEST_F(TupleStreamWrapperTest, FarFutureFlush)
{
    for (int i = 1; i < 10; i++)
    {
        appendTuple(i-1, i, 0);
    }
    m_wrapper->periodicFlush(-1, 99, 100);

    for (int i = 100; i < 110; i++)
    {
        appendTuple(i-1, i, 0);
    }
    m_wrapper->periodicFlush(-1, 130, 131);

    // get the first buffer flushed
    ASSERT_TRUE(m_topend.receivedExportBuffer);
    shared_ptr<StreamBlock> results = m_topend.blocks.front();
    m_topend.blocks.pop_front();
    EXPECT_EQ(results->uso(), 0);
    EXPECT_EQ(results->generationId(), 0);
    EXPECT_EQ(results->offset(), (MAGIC_TUPLE_SIZE * 9));

    // now get the second
    ASSERT_FALSE(m_topend.blocks.empty());
    results = m_topend.blocks.front();
    m_topend.blocks.pop_front();
    EXPECT_EQ(results->uso(), (MAGIC_TUPLE_SIZE * 9));
    EXPECT_EQ(results->generationId(), 0);
    EXPECT_EQ(results->offset(), (MAGIC_TUPLE_SIZE * 10));
}

/**
 * Fill a buffer by appending tuples that advance the last committed TXN
 */
TEST_F(TupleStreamWrapperTest, Fill) {

    int tuples_to_fill = BUFFER_SIZE / MAGIC_TUPLE_SIZE;
    // fill with just enough tuples to avoid exceeding buffer
    for (int i = 1; i <= tuples_to_fill; i++)
    {
        appendTuple(i-1, i, 0);
    }
    // We shouldn't yet get a buffer because we haven't forced the
    // generation of a new one by exceeding the current one.
    ASSERT_FALSE(m_topend.receivedExportBuffer);

    // now, drop in one more
    appendTuple(tuples_to_fill, tuples_to_fill + 1, 0);

    ASSERT_TRUE(m_topend.receivedExportBuffer);
    shared_ptr<StreamBlock> results = m_topend.blocks.front();
    m_topend.blocks.pop_front();
    EXPECT_EQ(results->uso(), 0);
    EXPECT_EQ(results->generationId(), 0);
    EXPECT_EQ(results->offset(), (MAGIC_TUPLE_SIZE * tuples_to_fill));
}

/**
 * Fill a buffer with a single TXN, and then finally close it in the next
 * buffer.
 */
TEST_F(TupleStreamWrapperTest, FillSingleTxnAndAppend) {

    int tuples_to_fill = BUFFER_SIZE / MAGIC_TUPLE_SIZE;
    // fill with just enough tuples to avoid exceeding buffer
    for (int i = 1; i <= tuples_to_fill; i++)
    {
        appendTuple(0, 1, 0);
    }
    // We shouldn't yet get a buffer because we haven't forced the
    // generation of a new one by exceeding the current one.
    ASSERT_FALSE(m_topend.receivedExportBuffer);

    // now, drop in one more on the same TXN ID
    appendTuple(0, 1, 0);

    // We shouldn't yet get a buffer because we haven't closed the current
    // transaction
    ASSERT_FALSE(m_topend.receivedExportBuffer);

    // now, finally drop in a tuple that closes the first TXN
    appendTuple(1, 2, 0);

    ASSERT_TRUE(m_topend.receivedExportBuffer);
    shared_ptr<StreamBlock> results = m_topend.blocks.front();
    m_topend.blocks.pop_front();
    EXPECT_EQ(results->uso(), 0);
    EXPECT_EQ(results->generationId(), 0);
    EXPECT_EQ(results->offset(), (MAGIC_TUPLE_SIZE * tuples_to_fill));
}

/**
 * Fill a buffer with a single TXN, and then finally close it in the next
 * buffer using periodicFlush
 */
TEST_F(TupleStreamWrapperTest, FillSingleTxnAndFlush) {

    int tuples_to_fill = BUFFER_SIZE / MAGIC_TUPLE_SIZE;
    // fill with just enough tuples to avoid exceeding buffer
    for (int i = 1; i <= tuples_to_fill; i++)
    {
        appendTuple(0, 1, 0);
    }
    // We shouldn't yet get a buffer because we haven't forced the
    // generation of a new one by exceeding the current one.
    ASSERT_FALSE(m_topend.receivedExportBuffer);

    // now, drop in one more on the same TXN ID
    appendTuple(0, 1, 0);

    // We shouldn't yet get a buffer because we haven't closed the current
    // transaction
    ASSERT_FALSE(m_topend.receivedExportBuffer);

    // Now, flush the buffer with the tick
    m_wrapper->periodicFlush(-1, 1, 1);

    // should be able to get 2 buffers, one full and one with one tuple
    ASSERT_TRUE(m_topend.receivedExportBuffer);
    shared_ptr<StreamBlock> results = m_topend.blocks.front();
    m_topend.blocks.pop_front();
    EXPECT_EQ(results->uso(), 0);
    EXPECT_EQ(results->generationId(), 0);
    EXPECT_EQ(results->offset(), (MAGIC_TUPLE_SIZE * tuples_to_fill));

    results = m_topend.blocks.front();
    m_topend.blocks.pop_front();
    EXPECT_EQ(results->uso(), (MAGIC_TUPLE_SIZE * tuples_to_fill));
    EXPECT_EQ(results->generationId(), 0);
    EXPECT_EQ(results->offset(), MAGIC_TUPLE_SIZE);
}

/**
 * Fill a buffer with a single TXN, close it with the first tuple in
 * the next buffer, and then roll back that tuple, and verify that our
 * committed buffer is still there.
 */
TEST_F(TupleStreamWrapperTest, FillSingleTxnAndCommitWithRollback) {

    int tuples_to_fill = BUFFER_SIZE / MAGIC_TUPLE_SIZE;
    // fill with just enough tuples to avoid exceeding buffer
    for (int i = 1; i <= tuples_to_fill; i++)
    {
        appendTuple(0, 1, 0);
    }
    // We shouldn't yet get a buffer because we haven't forced the
    // generation of a new one by exceeding the current one.
    ASSERT_FALSE(m_topend.receivedExportBuffer);

    // now, drop in one more on a new TXN ID.  This should commit
    // the whole first buffer.  Roll back the new tuple and make sure
    // we have a good buffer
    size_t mark = m_wrapper->bytesUsed();
    appendTuple(1, 2, 0);
    m_wrapper->rollbackTo(mark);

    // so flush and make sure we got something sane
    m_wrapper->periodicFlush(-1, 1, 2);
    ASSERT_TRUE(m_topend.receivedExportBuffer);
    shared_ptr<StreamBlock> results = m_topend.blocks.front();
    m_topend.blocks.pop_front();
    EXPECT_EQ(results->uso(), 0);
    EXPECT_EQ(results->generationId(), 0);
    EXPECT_EQ(results->offset(), (MAGIC_TUPLE_SIZE * tuples_to_fill));
}

/**
 * Verify that several filled buffers all with one open transaction returns
 * nada.
 */
TEST_F(TupleStreamWrapperTest, FillWithOneTxn) {

    int tuples_to_fill = BUFFER_SIZE / MAGIC_TUPLE_SIZE;
    // fill several buffers
    for (int i = 0; i <= (tuples_to_fill + 10) * 3; i++)
    {
        appendTuple(1, 2, 0);
    }
    // We shouldn't yet get a buffer even though we've filled a bunch because
    // the transaction is still open.
    ASSERT_FALSE(m_topend.receivedExportBuffer);
}

/**
 * Simple rollback test, verify that we can rollback the first tuple,
 * append another tuple, and only get one tuple in the output buffer.
 */
TEST_F(TupleStreamWrapperTest, RollbackFirstTuple)
{

    appendTuple(1, 2, 0);
    // rollback the first tuple
    m_wrapper->rollbackTo(0);

    // write a new tuple and then flush the buffer
    appendTuple(1, 3, 5);
    m_wrapper->periodicFlush(-1, 3, 3);

    // we should only have one tuple in the buffer
    ASSERT_TRUE(m_topend.receivedExportBuffer);
    shared_ptr<StreamBlock> results = m_topend.blocks.front();
    m_topend.blocks.pop_front();
    EXPECT_EQ(results->uso(), 0);
    // We wiped out the original tuple, so the generation ID of the first tuple
    // should be the tuple we replaced it with
    EXPECT_EQ(results->generationId(), 5);
    EXPECT_EQ(results->offset(), MAGIC_TUPLE_SIZE);
}


/**
 * Another simple rollback test, verify that a tuple in the middle of
 * a buffer can get rolled back and leave the committed transaction
 * untouched.
 */
TEST_F(TupleStreamWrapperTest, RollbackMiddleTuple)
{
    // append a bunch of tuples
    for (int i = 1; i <= 10; i++)
    {
        appendTuple(i-1, i, 0);
    }

    // add another and roll it back and flush
    size_t mark = m_wrapper->bytesUsed();
    appendTuple(10, 11, 0);
    m_wrapper->rollbackTo(mark);
    m_wrapper->periodicFlush(-1, 10, 11);

    ASSERT_TRUE(m_topend.receivedExportBuffer);
    shared_ptr<StreamBlock> results = m_topend.blocks.front();
    m_topend.blocks.pop_front();
    EXPECT_EQ(results->uso(), 0);
    EXPECT_EQ(results->generationId(), 0);
    EXPECT_EQ(results->offset(), (MAGIC_TUPLE_SIZE * 10));
}

/**
 * Verify that a transaction can generate entire buffers, they can all
 * be rolled back, and the original committed bytes are untouched.
 */
TEST_F(TupleStreamWrapperTest, RollbackWholeBuffer)
{
    // append a bunch of tuples
    for (int i = 1; i <= 10; i++)
    {
        appendTuple(i-1, i, 0);
    }

    // now, fill a couple of buffers with tuples from a single transaction
    size_t mark = m_wrapper->bytesUsed();
    int tuples_to_fill = BUFFER_SIZE / MAGIC_TUPLE_SIZE;
    for (int i = 0; i < (tuples_to_fill + 10) * 2; i++)
    {
        appendTuple(10, 11, 0);
    }
    m_wrapper->rollbackTo(mark);
    m_wrapper->periodicFlush(-1, 10, 11);

    ASSERT_TRUE(m_topend.receivedExportBuffer);
    shared_ptr<StreamBlock> results = m_topend.blocks.front();
    m_topend.blocks.pop_front();
    EXPECT_EQ(results->uso(), 0);
    EXPECT_EQ(results->generationId(), 0);
    EXPECT_EQ(results->offset(), (MAGIC_TUPLE_SIZE * 10));
}

/**
 * Verify that advancing the exportWindow generates a new buffer
 */
TEST_F(TupleStreamWrapperTest, AdvanceExportWindow)
{
    for (int i = 1; i < 10; i++)
    {
        appendTuple(i-1, i, 0);
    }
    appendTuple(10, 11, 1);
    m_wrapper->periodicFlush(-1, 11, 11);
    ASSERT_TRUE(m_topend.receivedEndOfStream);

    // get the first buffer flushed
    ASSERT_TRUE(m_topend.receivedExportBuffer);
    shared_ptr<StreamBlock> results = m_topend.blocks.front();
    m_topend.blocks.pop_front();
    EXPECT_EQ(results->uso(), 0);
    EXPECT_EQ(results->generationId(), 0);
    EXPECT_EQ(results->offset(), (MAGIC_TUPLE_SIZE * 9));

    // now get the second
    ASSERT_FALSE(m_topend.blocks.empty());
    results = m_topend.blocks.front();
    m_topend.blocks.pop_front();
    EXPECT_EQ(results->uso(), (MAGIC_TUPLE_SIZE * 9));
    EXPECT_EQ(results->generationId(), 1);
    EXPECT_EQ(results->offset(), MAGIC_TUPLE_SIZE);
}

/**
 * Verify that a catalog update (setGenerationAndSignature)
 * results in a new buffer
 */
TEST_F(TupleStreamWrapperTest, CatalogUpdateTest)
{
    for (int i = 1; i < 10; i++)
    {
        appendTuple(i-1, i, 0);
    }
    appendTuple(10, 11, 0);
    ASSERT_FALSE(m_topend.receivedEndOfStream);
    m_wrapper->setSignatureAndGeneration("dude", 12);
    appendTuple(12, 13, 10);
    m_wrapper->periodicFlush(-1, 13, 13);
    ASSERT_TRUE(m_topend.receivedEndOfStream);

    // get the first buffer flushed
    ASSERT_TRUE(m_topend.receivedExportBuffer);
    shared_ptr<StreamBlock> results = m_topend.blocks.front();
    m_topend.blocks.pop_front();
    EXPECT_EQ(results->uso(), 0);
    EXPECT_EQ(results->generationId(), 0);
    EXPECT_EQ(results->offset(), (MAGIC_TUPLE_SIZE * 10));

    // now get the second
    ASSERT_FALSE(m_topend.blocks.empty());
    results = m_topend.blocks.front();
    m_topend.blocks.pop_front();
    EXPECT_EQ(results->uso(), (MAGIC_TUPLE_SIZE * 10));
    EXPECT_EQ(results->generationId(), 12);
    EXPECT_EQ(results->offset(), MAGIC_TUPLE_SIZE);
}

TEST_F(TupleStreamWrapperTest, CatalogUpdateAfterFlush)
{
    for (int i = 1; i < 10; i++)
    {
        appendTuple(i-1, i, 0);
    }
    m_wrapper->periodicFlush(-1, 10, 10);
    ASSERT_FALSE(m_topend.receivedEndOfStream);
    m_wrapper->setSignatureAndGeneration("dude", 12);
    appendTuple(12, 13, 10);
    m_wrapper->periodicFlush(-1, 13, 13);
    ASSERT_TRUE(m_topend.receivedEndOfStream);

    // get the first buffer flushed
    ASSERT_TRUE(m_topend.receivedExportBuffer);
    shared_ptr<StreamBlock> results = m_topend.blocks.front();
    m_topend.blocks.pop_front();
    EXPECT_EQ(results->uso(), 0);
    EXPECT_EQ(results->generationId(), 0);
    EXPECT_EQ(results->offset(), (MAGIC_TUPLE_SIZE * 9));

    // now get the second
    ASSERT_FALSE(m_topend.blocks.empty());
    results = m_topend.blocks.front();
    m_topend.blocks.pop_front();
    EXPECT_EQ(results->uso(), (MAGIC_TUPLE_SIZE * 9));
    EXPECT_EQ(results->generationId(), 12);
    EXPECT_EQ(results->offset(), MAGIC_TUPLE_SIZE);
}

TEST_F(TupleStreamWrapperTest, CatalogUpdateAfterRollback)
{
    for (int i = 1; i < 10; i++)
    {
        appendTuple(i-1, i, 0);
    }
    ASSERT_FALSE(m_topend.receivedEndOfStream);
    size_t mark = m_wrapper->bytesUsed();
    appendTuple(10, 11, 4);
    // This should trip a new buffer despite getting rolled back.
    m_wrapper->rollbackTo(mark);
    // Then, we should end up with THIS as our generation ID
    m_wrapper->setSignatureAndGeneration("dude", 12);
    appendTuple(12, 13, 10);
    m_wrapper->periodicFlush(-1, 13, 13);
    ASSERT_TRUE(m_topend.receivedEndOfStream);

    // get the first buffer flushed
    ASSERT_TRUE(m_topend.receivedExportBuffer);
    shared_ptr<StreamBlock> results = m_topend.blocks.front();
    m_topend.blocks.pop_front();
    EXPECT_EQ(results->uso(), 0);
    EXPECT_EQ(results->generationId(), 0);
    EXPECT_EQ(results->offset(), (MAGIC_TUPLE_SIZE * 9));

    // now get the second
    ASSERT_FALSE(m_topend.blocks.empty());
    results = m_topend.blocks.front();
    m_topend.blocks.pop_front();
    EXPECT_EQ(results->uso(), (MAGIC_TUPLE_SIZE * 9));
    EXPECT_EQ(results->generationId(), 12);
    EXPECT_EQ(results->offset(), MAGIC_TUPLE_SIZE);
}

TEST_F(TupleStreamWrapperTest, PeriodicFlushEndOfStream)
{
    // write a new tuple and then flush the buffer
    appendTuple(1, 2, 0);
    m_wrapper->periodicFlush(-1, 2, 2);
    appendTuple(2, 3, 1);
    m_wrapper->periodicFlush(-1, 3, 3);

    // we should only have one tuple in the buffer
    ASSERT_TRUE(m_topend.receivedExportBuffer);
    // And we should have seen some kind of end of stream indication
    ASSERT_TRUE(m_topend.receivedEndOfStream);
    shared_ptr<StreamBlock> results = m_topend.blocks.front();
    m_topend.blocks.pop_front();
    EXPECT_EQ(results->uso(), 0);
    EXPECT_EQ(results->generationId(), 0);
    EXPECT_EQ(results->offset(), MAGIC_TUPLE_SIZE);
}

TEST_F(TupleStreamWrapperTest, JustGenerationChange)
{
    m_wrapper->setSignatureAndGeneration("dude", 3);

    // No buffer
    ASSERT_TRUE(m_topend.blocks.empty());
    ASSERT_TRUE(m_topend.receivedExportBuffer);
    // But we should have seen some kind of end of stream indication
    ASSERT_TRUE(m_topend.receivedEndOfStream);
}

int main() {
    return TestSuite::globalInstance()->runAll();
}

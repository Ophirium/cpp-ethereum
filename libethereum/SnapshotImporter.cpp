/*
	This file is part of cpp-ethereum.

	cpp-ethereum is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	cpp-ethereum is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with cpp-ethereum.  If not, see <http://www.gnu.org/licenses/>.
*/
/** @file 
 */

#include "SnapshotImporter.h"
#include "Client.h"

#include <libdevcore/RLP.h>
#include <libdevcore/TrieHash.h>
#include <libethashseal/Ethash.h>

#include <deps/include/snappy.h>

using namespace dev;
using namespace eth;

void SnapshotImporter::import(std::string const& _snapshotDirPath)
{
	boost::filesystem::path const snapshotDir(_snapshotDirPath);

	// TODO handle read failure
	bytes manifestBytes = dev::contents((snapshotDir / "MANIFEST").string());
	RLP manifest(manifestBytes);

	u256 version = manifest[0].toInt<u256>();
	if (version != 2)
		BOOST_THROW_EXCEPTION(UnsupportedSnapshotManifestVersion());


	h256s const stateChunkHashes = manifest[1].toVector<h256>();
	h256s const blockChunkHashes = manifest[2].toVector<h256>();

	h256 const stateRoot = manifest[3].toHash<h256>();
	//u256 const blockNumber = manifest[4].toInt<u256>();
	//h256 const blockHash = manifest[5].toHash<h256>();

	importStateChunks(snapshotDir, stateChunkHashes, stateRoot);
	importBlocks(snapshotDir, blockChunkHashes);
}

void SnapshotImporter::importStateChunks(boost::filesystem::path const& _snapshotDir, h256s const& _stateChunkHashes, h256 const& _stateRoot)
{
	size_t const stateChunkCount = _stateChunkHashes.size();
	size_t imported = 0;

	StateImporter stateImporter = m_client.createStateImporter();
	std::map<h256, bytes> storageMap;
	h256 addressHash;
	u256 nonce;
	u256 balance;
	h256 codeHash;
	std::string chunkUncompressed;
	size_t accountsImported = 0;
	for (auto const& stateHash : _stateChunkHashes)
	{
		std::string const chunkCompressed = dev::contentsString((_snapshotDir / toHex(stateHash)).string());

		h256 const chunkHash = sha3(chunkCompressed);
		assert(chunkHash == stateHash);

		bool success = snappy::Uncompress(chunkCompressed.data(), chunkCompressed.size(), &chunkUncompressed);
		assert(success);

		RLP accounts(chunkUncompressed);

		//size_t accs = accounts.itemCount();
		for (auto addressAndAccount : accounts)
		{
			assert(addressAndAccount.itemCount() == 2);
			h256 addressHashNew = addressAndAccount[0].toHash<h256>();
			assert(storageMap.empty() || addressHash == addressHashNew);
			addressHash = addressHashNew;
			assert(addressHash);

			RLP const account = addressAndAccount[1];
			assert(account.itemCount() == 5);

			assert(storageMap.empty() || nonce == account[0].toInt<u256>());
			nonce = account[0].toInt<u256>();
			assert(storageMap.empty() || balance == account[1].toInt<u256>());
			balance = account[1].toInt<u256>();

			RLP const storage = account[4];
			//int const storageCount = storage.itemCount();

			RLPStream s(4);
			s << nonce << balance;

			assert(storageMap.empty() || codeHash == account[3].toHash<h256>());

			for (auto hashAndValue : storage)
			{
				assert(hashAndValue.itemCount() == 2);
				h256 const keyHash = hashAndValue[0].toHash<h256>();
				assert(keyHash);
				bytes value = hashAndValue[1].toBytes();
				assert(RLP(value).isInt());

				assert(storageMap.find(keyHash) == storageMap.end());
				storageMap.emplace(keyHash, std::move(value));
			}

			byte const codeFlag = account[2].toInt<byte>();

			switch (codeFlag)
			{
			case 0:
				codeHash = EmptySHA3;
				break;
			case 1:
				codeHash = stateImporter.importCode(account[3].toBytesConstRef());
				break;
			case 2:
				codeHash = account[3].toHash<h256>();
				assert(codeHash);
				assert(!stateImporter.lookupCode(codeHash).empty());
				break;
			default:
				BOOST_THROW_EXCEPTION(InvalidStateChunkData());
			}

			if (storage.itemCount() < 80000)
			{
				//					assert(nonce != 0 || balance != 0 || codeHash != EmptySHA3);
				stateImporter.importAccount(addressHash, nonce, balance, storageMap, codeHash);
				++accountsImported;
				storageMap.clear();
			}
		}

		stateImporter.commitStateDatabase();
		clog(SnapshotImportLog) << "Imported chunk " << stateHash << " (" << accounts.itemCount() << " accounts)\n";
		clog(SnapshotImportLog) << stateChunkCount - (++imported) << " chunks left to import\n";
	}

	// check root
	clog(SnapshotImportLog) << "Chunks imported: " << imported << "\n";
	clog(SnapshotImportLog) << "Accounts imported: " << accountsImported << "\n";
	clog(SnapshotImportLog) << "Reconstructed state root: " << stateImporter.stateRoot() << "\n";
	clog(SnapshotImportLog) << "Manifest state root:      " << _stateRoot << "\n";
	if (stateImporter.stateRoot() != _stateRoot)
		BOOST_THROW_EXCEPTION(StateTrieReconstructionFailed());
}

void SnapshotImporter::importBlocks(boost::filesystem::path const& _snapshotDir, h256s const& _blockChunkHashes)
{
	BlockChainImporter bcImporter = m_client.createBlockChainImporter();

	size_t const blockChunkCount = _blockChunkHashes.size();
	size_t blockChunksImported = 0;
	for (auto blockChunkHash : _blockChunkHashes)
	{
		std::string const chunkCompressed = dev::contentsString((_snapshotDir / toHex(blockChunkHash)).string());

		h256 const chunkHash = sha3(chunkCompressed);
		assert(chunkHash == blockChunkHash);

		std::string chunkUncompressed;
		bool success = snappy::Uncompress(chunkCompressed.data(), chunkCompressed.size(), &chunkUncompressed);
		assert(success);

		RLP blockChunk(chunkUncompressed);
		u256 const firstBlockNumber = blockChunk[0].toInt<u256>();
		assert(firstBlockNumber);
		h256 const firstBlockHash = blockChunk[1].toHash<h256>();
		assert(firstBlockHash);
		u256 const firstBlockDifficulty = blockChunk[2].toInt<u256>();
		assert(firstBlockDifficulty);

		clog(SnapshotImportLog) << "chunk first block " << firstBlockNumber << " first block hash " << firstBlockHash << " difficulty " << firstBlockDifficulty << "\n";

		size_t const itemCount = blockChunk.itemCount();
		h256 parentHash = firstBlockHash;
		u256 number = firstBlockNumber + 1;
		u256 totalDifficulty = firstBlockDifficulty;
		for (size_t i = 3; i < itemCount; ++i, ++number)
		{
			RLP blockAndReceipts = blockChunk[i];
			assert(blockAndReceipts.itemCount() == 2);
			RLP abridgedBlock = blockAndReceipts[0];


			BlockHeader header;
			header.setParentHash(parentHash);
			header.setAuthor(abridgedBlock[0].toHash<Address>(RLP::VeryStrict));

			h256 const blockStateRoot = abridgedBlock[1].toHash<h256>(RLP::VeryStrict);
			RLP transactions = abridgedBlock[8];
			h256 const txRoot = trieRootOver(transactions.itemCount(), [&](unsigned i) { return rlp(i); }, [&](unsigned i) { return transactions[i].data().toBytes(); });
			RLP uncles = abridgedBlock[9];
			RLP receipts = blockAndReceipts[1];
			std::vector<bytesConstRef> receiptsVector;
			for (auto receipt : receipts)
				receiptsVector.push_back(receipt.data());
			h256 const receiptsRoot = orderedTrieRoot(receiptsVector);
			h256 const unclesHash = sha3(uncles.data());
			header.setRoots(txRoot, receiptsRoot, unclesHash, blockStateRoot);

			header.setLogBloom(abridgedBlock[2].toHash<LogBloom>(RLP::VeryStrict));
			u256 const difficulty = abridgedBlock[3].toInt<u256>(RLP::VeryStrict);
			header.setDifficulty(difficulty);
			header.setNumber(number);
			header.setGasLimit(abridgedBlock[4].toInt<u256>(RLP::VeryStrict));
			header.setGasUsed(abridgedBlock[5].toInt<u256>(RLP::VeryStrict));
			header.setTimestamp(abridgedBlock[6].toInt<u256>(RLP::VeryStrict));
			header.setExtraData(abridgedBlock[7].toBytes(RLP::VeryStrict));

			Ethash::setMixHash(header, abridgedBlock[10].toHash<h256>(RLP::VeryStrict));
			Ethash::setNonce(header, abridgedBlock[11].toHash<Nonce>(RLP::VeryStrict));

			totalDifficulty += difficulty;
			bcImporter.importBlock(header, transactions, uncles, receipts, number, totalDifficulty);

			parentHash = header.hash();
			//				cout << "i = " << i << " author " << author << " state root " <<  blockStateRoot << " timestamp " << timestamp << endl;

		}

		clog(SnapshotImportLog) << "Imported chunk " << blockChunkHash << " (" << itemCount - 3 << " blocks)\n";
		clog(SnapshotImportLog) << blockChunkCount - (++blockChunksImported) << " chunks left to import\n";
	}
}

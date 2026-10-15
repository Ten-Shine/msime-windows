#include "tests/includes/test_framework.h"
#include "tests/includes/test_utf8_path.h"
#include "engine/english/english_dictionary.h"
#include "engine/english/translation_gloss.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <sqlite3.h>
#include <stdexcept>
#include <string>

namespace
{
class TemporaryEnglishDatabase
{
  public:
    TemporaryEnglishDatabase()
    {
        const auto suffix = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        directory_ = std::filesystem::temp_directory_path() / ("metasequoia_english_test_" + suffix);
        std::filesystem::create_directories(directory_);
        path_ = directory_ / "english.db";

        sqlite3 *db = nullptr;
        if (sqlite3_open(test::Utf8(path_).c_str(), &db) != SQLITE_OK)
        {
            const std::string error = db == nullptr ? "unknown error" : sqlite3_errmsg(db);
            if (db != nullptr)
            {
                sqlite3_close(db);
            }
            throw std::runtime_error("Unable to create English test database: " + error);
        }

        constexpr const char *sql = "CREATE TABLE english_words ("
                                    "word TEXT PRIMARY KEY COLLATE BINARY, display TEXT NOT NULL"
                                    ") WITHOUT ROWID;"
                                    "INSERT INTO english_words VALUES ('hel', 'hel');"
                                    "INSERT INTO english_words VALUES ('held', 'held');"
                                    "INSERT INTO english_words VALUES ('hello', 'hello');"
                                    "INSERT INTO english_words VALUES ('help', 'help');"
                                    "INSERT INTO english_words VALUES ('helpful', 'helpful');"
                                    "INSERT INTO english_words VALUES ('hero', 'hero');";
        char *error_message = nullptr;
        if (sqlite3_exec(db, sql, nullptr, nullptr, &error_message) != SQLITE_OK)
        {
            const std::string error = error_message == nullptr ? "unknown error" : error_message;
            sqlite3_free(error_message);
            sqlite3_close(db);
            throw std::runtime_error("Unable to populate English test database: " + error);
        }
        sqlite3_close(db);
    }

    ~TemporaryEnglishDatabase()
    {
        std::error_code error;
        std::filesystem::remove_all(directory_, error);
    }

    const std::filesystem::path &path() const
    {
        return path_;
    }

  private:
    std::filesystem::path directory_;
    std::filesystem::path path_;
};
} // namespace

TEST_CASE(EnglishDictionaryOrdersByExactMatchThenLengthAndWord)
{
    TemporaryEnglishDatabase database;
    EnglishDictionary dictionary(test::Utf8(database.path()));

    const auto candidates = dictionary.query_prefix("hel", 4);
    REQUIRE_EQ(candidates.size(), static_cast<size_t>(4));
    REQUIRE_EQ(candidates[0].word, std::string("hel"));
    REQUIRE_EQ(candidates[1].word, std::string("held"));
    REQUIRE_EQ(candidates[2].word, std::string("help"));
    REQUIRE_EQ(candidates[3].word, std::string("hello"));
    REQUIRE_EQ(candidates[0].pinyin, std::string("hel"));
    REQUIRE(candidates[0].source == CandidateSource::EnglishDictionary);
}

TEST_CASE(EnglishDictionaryRestrictsResultsToTheRequestedPrefix)
{
    TemporaryEnglishDatabase database;
    EnglishDictionary dictionary(test::Utf8(database.path()));

    const auto candidates = dictionary.query_prefix("help", 10);
    REQUIRE_EQ(candidates.size(), static_cast<size_t>(2));
    REQUIRE_EQ(candidates[0].word, std::string("help"));
    REQUIRE_EQ(candidates[1].word, std::string("helpful"));
    REQUIRE(dictionary.query_prefix("HEL", 5).empty());
}

TEST_CASE(EnglishDictionaryMigratesLegacySchemaAndSupportsMultipleDisplaysPerKey)
{
    TemporaryEnglishDatabase database;
    EnglishDictionary dictionary(test::Utf8(database.path()));

    sqlite3 *db = nullptr;
    REQUIRE_EQ(sqlite3_open(test::Utf8(database.path()).c_str(), &db), SQLITE_OK);
    REQUIRE_EQ(sqlite3_exec(db, "INSERT INTO english_words(word,display,weight) VALUES('hello','Hello',100);", nullptr,
                            nullptr, nullptr),
               SQLITE_OK);
    sqlite3_close(db);

    const auto candidates = dictionary.query_prefix("hello", 10);
    REQUIRE_EQ(candidates.size(), static_cast<size_t>(2));
    REQUIRE_EQ(candidates[0].pinyin, std::string("hello"));
    REQUIRE_EQ(candidates[0].word, std::string("Hello"));
    REQUIRE_EQ(candidates[1].word, std::string("hello"));
}

TEST_CASE(EnglishDictionaryQueriesBothGlossDirections)
{
    TemporaryEnglishDatabase database;
    EnglishDictionary dictionary(test::Utf8(database.path()));

    sqlite3 *db = nullptr;
    REQUIRE_EQ(sqlite3_open(test::Utf8(database.path()).c_str(), &db), SQLITE_OK);
    REQUIRE_EQ(sqlite3_exec(db,
                            "INSERT INTO en_zh_glosses VALUES('implement','实现；执行');"
                            "INSERT INTO zh_en_glosses VALUES('实现','realize; implement');",
                            nullptr, nullptr, nullptr),
               SQLITE_OK);
    sqlite3_close(db);

    REQUIRE_EQ(dictionary.query_chinese_gloss("implement"), std::string("实现；执行"));
    REQUIRE_EQ(dictionary.query_english_gloss("实现"), std::string("realize; implement"));
    REQUIRE(dictionary.query_chinese_gloss("missing").empty());
}

TEST_CASE(EnglishDictionaryMergesSiblingCustomTranslationsInsteadOfOverriding)
{
    TemporaryEnglishDatabase database;
    sqlite3 *db = nullptr;
    REQUIRE_EQ(sqlite3_open(test::Utf8(database.path()).c_str(), &db), SQLITE_OK);
    REQUIRE_EQ(sqlite3_exec(db,
                            "CREATE TABLE IF NOT EXISTS zh_en_glosses("
                            "chinese TEXT COLLATE BINARY PRIMARY KEY,english_gloss TEXT NOT NULL) WITHOUT ROWID;"
                            "INSERT INTO zh_en_glosses VALUES('华科','Huazhong University of');",
                            nullptr, nullptr, nullptr),
               SQLITE_OK);
    sqlite3_close(db);

    const auto sidecar = database.path().parent_path() / "custom_translations.txt";
    {
        std::ofstream output(sidecar, std::ios::binary);
        output << "华科\tHuazhong University of Science and Technology\n";
        output << "hust\t华中科技大学\n";
    }

    EnglishDictionary dictionary(test::Utf8(database.path()));
    // 内置释义在前，自定义里不重复的义项追加在后，不再整条顶掉内置释义。
    REQUIRE_EQ(dictionary.query_english_gloss("华科"),
               std::string("Huazhong University of; Huazhong University of Science and Technology"));
    // 内置没有这个键时，自定义释义就是全部。
    REQUIRE_EQ(dictionary.query_chinese_gloss("hust"), std::string("华中科技大学"));
    std::error_code error;
    std::filesystem::remove(sidecar, error);
}

TEST_CASE(EnglishDictionaryDeduplicatesMergedTranslations)
{
    TemporaryEnglishDatabase database;
    sqlite3 *db = nullptr;
    REQUIRE_EQ(sqlite3_open(test::Utf8(database.path()).c_str(), &db), SQLITE_OK);
    REQUIRE_EQ(sqlite3_exec(db,
                            "CREATE TABLE IF NOT EXISTS zh_en_glosses("
                            "chinese TEXT COLLATE BINARY PRIMARY KEY,english_gloss TEXT NOT NULL) WITHOUT ROWID;"
                            "CREATE TABLE IF NOT EXISTS en_zh_glosses("
                            "english TEXT COLLATE BINARY PRIMARY KEY,chinese_gloss TEXT NOT NULL) WITHOUT ROWID;"
                            "INSERT INTO zh_en_glosses VALUES('笑','adj. spreading; n. sound of laughter');"
                            "INSERT INTO zh_en_glosses VALUES('银牌','silver medal');"
                            "INSERT INTO zh_en_glosses VALUES('大笑','laugh');"
                            "INSERT INTO en_zh_glosses VALUES('silver medal','银牌；Silver Medal');",
                            nullptr, nullptr, nullptr),
               SQLITE_OK);
    sqlite3_close(db);

    const auto sidecar = database.path().parent_path() / "custom_translations.txt";
    {
        std::ofstream output(sidecar, std::ios::binary);
        // 去掉词性后与内置的 "adj. spreading" 相同 —— 重复，自定义这条不进。
        output << "笑\tspreading\n";
        // 内置那条没有词性、自定义这条有 —— 重复，保留带词性的。
        output << "银牌\tn. silver medal\n";
        // 大小写不敏感地相同 —— 重复，内置那条留下。
        output << "silver medal\tsilver medal\n";
        // 不重复的新义项追加在最后。
        output << "大笑\tguffaw\n";
    }

    EnglishDictionary dictionary(test::Utf8(database.path()));
    REQUIRE_EQ(dictionary.query_english_gloss("笑"), std::string("adj. spreading; n. sound of laughter"));
    REQUIRE_EQ(dictionary.query_english_gloss("银牌"), std::string("n. silver medal"));
    REQUIRE_EQ(dictionary.query_english_gloss("大笑"), std::string("laugh; guffaw"));
    REQUIRE_EQ(dictionary.query_chinese_gloss("silver medal"), std::string("银牌；Silver Medal"));
    std::error_code error;
    std::filesystem::remove(sidecar, error);
}

// The merge rules live in the engine (engine/english/translation_gloss.h) because it is the side that
// joins the custom translations with the glosses built into english.db. The server interprets the same
// gloss format for display and Ctrl+Enter, so the two must agree on what a sense and a label are.
TEST_CASE(TranslationGlossMergeAppendsWithoutDuplicating)
{
    using translation_gloss::Merge;
    const auto separator = translation_gloss::kEnglishGlossSeparator;
    REQUIRE_EQ(Merge("silver coin", "silver piece", separator), std::string("silver coin; silver piece"));
    REQUIRE_EQ(Merge("", "guffaw", separator), std::string("guffaw"));
    REQUIRE_EQ(Merge("silver coin", "", separator), std::string("silver coin"));
    REQUIRE(Merge("", "", separator).empty());
    REQUIRE(Merge("; ", " ; ", separator).empty());
    REQUIRE_EQ(Merge("银牌；奖牌", "银牌", translation_gloss::kChineseGlossSeparator), std::string("银牌；奖牌"));
}

TEST_CASE(TranslationGlossMergeDeduplicatesByPartOfSpeechAndCase)
{
    using translation_gloss::Merge;
    const auto separator = translation_gloss::kEnglishGlossSeparator;
    // 去掉词性后相同即为重复，保留先出现的那条。
    REQUIRE_EQ(Merge("adj. spreading", "spreading", separator), std::string("adj. spreading"));
    REQUIRE_EQ(Merge("n. sound of laughter", "Sound of Laughter", separator), std::string("n. sound of laughter"));
    // 没有词性的一条先出现、带词性的那条后到：换成带词性的。
    REQUIRE_EQ(Merge("silver medal", "n. silver medal", separator), std::string("n. silver medal"));
    // 两条都带词性时保留先出现的。
    REQUIRE_EQ(Merge("n. silver medal", "v. silver medal", separator), std::string("n. silver medal"));
    // 同一条释义里的重复义项也去掉。
    REQUIRE_EQ(Merge("silver medal; Silver Medal", "", separator), std::string("silver medal"));
    // 只有标签、标签后面没有内容时不算标签，与显示侧一致。
    REQUIRE_EQ(Merge("n.", "n. ", separator), std::string("n."));
}

TEST_CASE(EnglishDictionaryUpsertsGlossForCloudFallback)
{
    TemporaryEnglishDatabase database;
    EnglishDictionary dictionary(test::Utf8(database.path()));
    REQUIRE(EnglishDictionary::upsert_gloss(test::Utf8(database.path()), true, "水杉", "metasequoia"));
    REQUIRE_EQ(dictionary.query_english_gloss("水杉"), std::string("metasequoia"));
    REQUIRE(EnglishDictionary::upsert_gloss(test::Utf8(database.path()), false, "metasequoia", "水杉"));
    REQUIRE_EQ(dictionary.query_chinese_gloss("metasequoia"), std::string("水杉"));
}

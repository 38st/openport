# Initial relative costs from GCC 13 ASan+UBSan run 37247318493, before fixture
# scaling. These priorities keep historically expensive tests out of the tail.
# COST and TEST_INCLUDE_FILES are supported CTest interfaces; never seed or edit
# CTest's private, writable Testing/Temporary/CTestCostData.txt cache.
# Discovery order (and therefore -I shard membership) remains unchanged.
function(openport_test_cost name seconds)
  if("${name}" IN_LIST openport_tests_TESTS)
    set_tests_properties("${name}" PROPERTIES COST "${seconds}")
  endif()
endfunction()

openport_test_cost(DemoFeed.GeneratedDividendsReachLiveSharesAndWarningsUnlessASourceIsExplicit 714.56)
openport_test_cost(DemoFeed.RotatesWithoutStoppedStatusPreservesIdsAndDeletesFiles 504.04)
openport_test_cost(ReplayHost.InterruptedMultiSessionRunKeepsSessionsDividendsAndVerifiesAtTheEnd 402.00)
openport_test_cost(MultiDayReplay.EvaluationDeadlineFailsAfterItsLastDateAndRestartReproducesIt 380.88)
openport_test_cost(ReproducibleRun.AmOpeningSettlementIsGatedByDriverAndNamesScenarioOrRecording 337.60)
openport_test_cost(MultiDayReplay.OneRunCarriesTheAccountThroughEverySessionAndItsJournalVerifies 329.83)
openport_test_cost(Backtest.GeneratedDaysMatchSingleReplayAndParallelReportsAreByteIdentical 310.17)
openport_test_cost(Backtest.JointGeneratedAccountSharesCapAndIsIdenticalAtOneAndEightWorkers 301.61)
openport_test_cost(Backtest.VolumeRuleAllowsOpeningFillsForGeneratedAndExplicitScenarioDays 226.03)
openport_test_cost(DemoFeed.RestartAfterLatestMainOrNamedJournalStillFills 224.87)
openport_test_cost(DemoMarket.LegacyRecordingsAreUnchanged 199.42)
openport_test_cost(DemoMarket.AmQuotesEndAtTheirLastRegularCloseBeforeCurbAndOvernight 185.98)
openport_test_cost(StressReplay.WideChainKeepsStrikesNearSpotAfterLargeMoveAndReachesMarginFloors 174.43)
openport_test_cost(TradingMarginAllocation.SeededMixedExpirySubadditivityRate 173.66)
openport_test_cost(DemoFeed.ConsecutiveDaysFillRollAndSettleWithLivePaperAccounts 144.99)

/**
 * Funded accounts and payouts model the second half of a prop firm's program.
 * The engine supports them, but this simulator funds no one, so the terminal
 * hides the funded plans and the Payouts page. An account that is already
 * funded (for example one started with `openportd --plan funded-...`) still
 * shows them. Set to true to offer funded plans again.
 */
export const showFundedAccounts = false

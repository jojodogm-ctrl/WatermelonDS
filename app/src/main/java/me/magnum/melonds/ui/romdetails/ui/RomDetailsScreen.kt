package me.magnum.melonds.ui.romdetails.ui

import android.view.KeyEvent
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.asPaddingValues
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.safeDrawing
import androidx.compose.foundation.layout.systemBarsPadding
import androidx.compose.foundation.pager.HorizontalPager
import androidx.compose.foundation.pager.rememberPagerState
import androidx.compose.material.Surface
import androidx.compose.material3.adaptive.currentWindowAdaptiveInfo
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.ui.Modifier
import androidx.compose.ui.focus.FocusRequester
import androidx.compose.ui.focus.focusRequester
import androidx.compose.ui.input.key.Key
import androidx.compose.ui.input.key.key
import androidx.compose.ui.input.key.onPreviewKeyEvent
import androidx.window.core.layout.WindowSizeClass
import kotlinx.coroutines.launch
import me.magnum.melonds.domain.model.rom.Rom
import me.magnum.melonds.ui.romdetails.model.OfflineAchievementsUiState
import me.magnum.melonds.ui.romdetails.model.RomConfigUiState
import me.magnum.melonds.ui.romdetails.model.RomConfigUpdateEvent
import me.magnum.melonds.ui.romdetails.model.RomDetailsTab
import me.magnum.melonds.ui.romdetails.model.RomRetroAchievementsUiState
import me.magnum.melonds.ui.theme.watermelon
import me.magnum.rcheevosapi.model.RAAchievement

@Composable
fun RomDetailsScreen(
    rom: Rom,
    boxArtUrl: String?,
    raCoverUrl: String?,
    romConfigUiState: RomConfigUiState,
    retroAchievementsUiState: RomRetroAchievementsUiState,
    offlineAchievementsUiState: OfflineAchievementsUiState,
    onNavigateBack: () -> Unit,
    onLaunchRom: (Rom) -> Unit,
    onRomConfigUpdate: (RomConfigUpdateEvent) -> Unit,
    onCustomInputConfigEdited: () -> Unit,
    onRetroAchievementsLogin: (username: String, password: String) -> Unit,
    onRetroAchievementsRetryLoad: () -> Unit,
    onViewAchievement: (RAAchievement) -> Unit,
    onOfflineSyncNow: () -> Unit,
    onSendSaveFile: () -> Unit,
    onImportSaveFile: () -> Unit,
    onNetplay: (host: Boolean, address: String, port: Int, players: Int) -> Unit,
    onAchievementFocused: (me.magnum.melonds.ui.common.achievements.ui.model.AchievementUiModel?) -> Unit = {},
    onSettingFocused: (String?, String?) -> Unit = { _, _ -> },
) {
    val colors = watermelon
    val pagerState = rememberPagerState(
        initialPage = RomDetailsTab.CONFIG.tabIndex,
        pageCount = { RomDetailsTab.entries.size },
    )
    androidx.compose.runtime.LaunchedEffect(pagerState.currentPage) {
        if (pagerState.currentPage != RomDetailsTab.RETRO_ACHIEVEMENTS.tabIndex) {
            onAchievementFocused(null)
        }
        if (pagerState.currentPage != RomDetailsTab.CONFIG.tabIndex) {
            onSettingFocused(null, null)
        }
    }
    val focusRequester = remember { FocusRequester() }
    val pageFocusRequesters = remember { List(RomDetailsTab.entries.size) { FocusRequester() } }
    val coroutineScope = rememberCoroutineScope()
    val navigateToTab = remember(coroutineScope, pagerState) {
        { tab: RomDetailsTab ->
            coroutineScope.launch {
                pagerState.animateScrollToPage(tab.tabIndex)
                pageFocusRequesters[tab.tabIndex].requestFocus()
            }
            Unit
        }
    }

    val windowSizeClass = currentWindowAdaptiveInfo().windowSizeClass
    val isLandscape = windowSizeClass.isWidthAtLeastBreakpoint(WindowSizeClass.WIDTH_DP_MEDIUM_LOWER_BOUND)

    val keyHandlingModifier = Modifier.onPreviewKeyEvent {
        if (it.nativeKeyEvent.action == KeyEvent.ACTION_DOWN) {
            when (it.key) {
                Key.ButtonL1 -> {
                    if (pagerState.currentPage > 0) {
                        navigateToTab(RomDetailsTab.entries[pagerState.currentPage - 1])
                        return@onPreviewKeyEvent true
                    }
                }
                Key.ButtonR1 -> {
                    if (pagerState.currentPage < RomDetailsTab.entries.lastIndex) {
                        navigateToTab(RomDetailsTab.entries[pagerState.currentPage + 1])
                        return@onPreviewKeyEvent true
                    }
                }
                Key.ButtonStart -> onLaunchRom(rom)
            }
        }
        false
    }

    val pagerContent: @Composable (contentPadding: PaddingValues) -> Unit = { contentPadding ->
        HorizontalPager(
            modifier = Modifier.fillMaxSize(),
            state = pagerState,
            userScrollEnabled = false,
        ) {
            val pageFocusRequester = pageFocusRequesters[it]
            when (it) {
                RomDetailsTab.CONFIG.tabIndex -> {
                    RomConfigUi(
                        modifier = Modifier.fillMaxSize().focusRequester(pageFocusRequester),
                        contentPadding = contentPadding,
                        rom = rom,
                        romConfigUiState = romConfigUiState,
                        onConfigUpdate = onRomConfigUpdate,
                        onCustomInputConfigEdited = onCustomInputConfigEdited,
                        onSettingFocused = { title, value -> onSettingFocused(title, value) },
                    )
                }
                RomDetailsTab.RETRO_ACHIEVEMENTS.tabIndex -> {
                    RomRetroAchievementsUi(
                        modifier = Modifier.fillMaxSize().focusRequester(pageFocusRequester),
                        contentPadding = contentPadding,
                        retroAchievementsUiState = retroAchievementsUiState,
                        offlineAchievementsUiState = offlineAchievementsUiState,
                        onLogin = onRetroAchievementsLogin,
                        onRetryLoad = onRetroAchievementsRetryLoad,
                        onViewAchievement = onViewAchievement,
                        onSyncOfflineNow = onOfflineSyncNow,
                        onAchievementFocused = { onAchievementFocused(it) },
                    )
                }
                RomDetailsTab.OFFLINE_ACHIEVEMENTS.tabIndex -> {
                    RomOfflineAchievementsUi(
                        modifier = Modifier.fillMaxSize().focusRequester(pageFocusRequester),
                        contentPadding = contentPadding,
                        offlineAchievementsUiState = offlineAchievementsUiState,
                        onSyncOfflineNow = onOfflineSyncNow,
                    )
                }
            }
        }
    }

    var showNetplayDialog by rememberSaveable { mutableStateOf(false) }
    if (showNetplayDialog) {
        NetplayDialog(
            onDismiss = { showNetplayDialog = false },
            onStart = { host, address, port, players ->
                showNetplayDialog = false
                onNetplay(host, address, port, players)
            },
        )
    }

    Surface(color = colors.bg, modifier = Modifier.fillMaxSize().then(keyHandlingModifier)) {
        if (isLandscape) {
            Row(Modifier.fillMaxSize().systemBarsPadding()) {
                RomHeroSidePanel(
                    rom = rom,
                    boxArtUrl = boxArtUrl,
                    raCoverUrl = raCoverUrl,
                    initialFocusRequester = focusRequester,
                    onLaunchRom = { onLaunchRom(rom) },
                    onNavigateBack = onNavigateBack,
                    onSendSaveFile = onSendSaveFile,
                    onImportSaveFile = onImportSaveFile,
                    onNetplay = { showNetplayDialog = true },
                )
                Column(Modifier.fillMaxSize().background(colors.bg)) {
                    RomDetailsTabRow(
                        currentTab = RomDetailsTab.entries[pagerState.currentPage],
                        onTabClicked = navigateToTab,
                    )
                    pagerContent(PaddingValues())
                }
            }
        } else {
            Column(Modifier.fillMaxSize()) {
                RomHeroVertical(
                    rom = rom,
                    boxArtUrl = boxArtUrl,
                    raCoverUrl = raCoverUrl,
                    initialFocusRequester = focusRequester,
                    onLaunchRom = { onLaunchRom(rom) },
                    onNavigateBack = onNavigateBack,
                    onSendSaveFile = onSendSaveFile,
                    onImportSaveFile = onImportSaveFile,
                    onNetplay = { showNetplayDialog = true },
                    modifier = Modifier.systemBarsPadding(),
                )
                RomDetailsTabRow(
                    currentTab = RomDetailsTab.entries[pagerState.currentPage],
                    onTabClicked = navigateToTab,
                )
                pagerContent(PaddingValues())
            }
        }

        LaunchedEffect(focusRequester) {
            focusRequester.requestFocus()
        }
    }
}

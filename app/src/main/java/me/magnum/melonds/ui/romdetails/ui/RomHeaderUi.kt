package me.magnum.melonds.ui.romdetails.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.interaction.MutableInteractionSource
import androidx.compose.foundation.interaction.collectIsFocusedAsState
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.IntrinsicSize
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.aspectRatio
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.DropdownMenu
import androidx.compose.material.DropdownMenuItem
import androidx.compose.material.Icon
import androidx.compose.material.Text
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material.icons.filled.PlayArrow
import androidx.compose.material.icons.filled.Save
import androidx.compose.material.icons.filled.Wifi
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.draw.shadow
import androidx.compose.ui.focus.FocusRequester
import androidx.compose.ui.focus.focusRequester
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import coil.compose.AsyncImage
import me.magnum.melonds.R
import me.magnum.melonds.domain.model.rom.Rom
import me.magnum.melonds.domain.model.rom.config.RomIconSource
import me.magnum.melonds.ui.romdetails.model.RomDetailsTab
import me.magnum.melonds.ui.romlist.composables.DsBoxArtAspectRatio
import me.magnum.melonds.ui.romlist.composables.RomMiniIcon
import me.magnum.melonds.ui.romlist.composables.ScanlinesOverlay
import me.magnum.melonds.ui.romlist.composables.WatermelonRomArt
import me.magnum.melonds.ui.romlist.composables.formatHoursLabel
import me.magnum.melonds.ui.romlist.composables.romDisplayName
import me.magnum.melonds.ui.romlist.composables.romGradient
import me.magnum.melonds.ui.romlist.composables.romInitials
import me.magnum.melonds.ui.romlist.composables.romPlatformLabel
import me.magnum.melonds.ui.theme.SpaceGrotesk
import me.magnum.melonds.ui.theme.WatermelonMono
import me.magnum.melonds.ui.theme.watermelon
import kotlin.time.Duration

@Composable
fun HeroCircleButton(
    icon: androidx.compose.ui.graphics.vector.ImageVector,
    contentDescription: String?,
    onClick: () -> Unit,
    modifier: Modifier = Modifier,
) {
    val interactionSource = remember { MutableInteractionSource() }
    val isFocused by interactionSource.collectIsFocusedAsState()
    val colors = watermelon
    Box(
        modifier = modifier
            .size(38.dp)
            .clip(CircleShape)
            .background(Color.Black.copy(alpha = 0.28f))
            .let { if (isFocused) it.background(colors.red.copy(alpha = 0.6f)) else it }
            .clickable(interactionSource = interactionSource, indication = null, onClick = onClick),
        contentAlignment = Alignment.Center,
    ) {
        Icon(icon, contentDescription = contentDescription, tint = Color.White, modifier = Modifier.size(20.dp))
    }
}

@Composable
fun PlayButton(
    height: Dp,
    onClick: () -> Unit,
    modifier: Modifier = Modifier,
    focusRequester: FocusRequester? = null,
) {
    val colors = watermelon
    val interactionSource = remember { MutableInteractionSource() }
    val isFocused by interactionSource.collectIsFocusedAsState()
    val shape = RoundedCornerShape(13.dp)
    Row(
        modifier = modifier
            .height(height)
            .shadow(if (isFocused) 16.dp else 8.dp, shape, spotColor = colors.redGlow)
            .clip(shape)
            .background(colors.red)
            .let { if (focusRequester != null) it.focusRequester(focusRequester) else it }
            .clickable(interactionSource = interactionSource, indication = null, onClick = onClick),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.Center,
    ) {
        Icon(Icons.Filled.PlayArrow, contentDescription = null, tint = Color.White, modifier = Modifier.size(19.dp))
        Spacer(Modifier.width(9.dp))
        Text(
            text = stringResource(R.string.play).uppercase(),
            color = Color.White,
            fontFamily = SpaceGrotesk,
            fontSize = 14.sp,
            fontWeight = FontWeight.SemiBold,
            letterSpacing = 0.3.sp,
        )
    }
}

@Composable
fun SaveActionsButton(
    size: Dp,
    onSendSaveFile: () -> Unit,
    onImportSaveFile: () -> Unit,
    modifier: Modifier = Modifier,
) {
    var menuOpen by remember { mutableStateOf(false) }
    val interactionSource = remember { MutableInteractionSource() }
    val isFocused by interactionSource.collectIsFocusedAsState()
    val colors = watermelon
    Box(modifier = modifier) {
        Box(
            modifier = Modifier
                .size(size)
                .clip(RoundedCornerShape(13.dp))
                .background(Color.Black.copy(alpha = 0.3f))
                .let { if (isFocused) it.background(colors.red.copy(alpha = 0.5f)) else it }
                .clickable(interactionSource = interactionSource, indication = null) { menuOpen = true },
            contentAlignment = Alignment.Center,
        ) {
            Icon(Icons.Filled.Save, contentDescription = null, tint = Color.White, modifier = Modifier.size(20.dp))
        }
        DropdownMenu(expanded = menuOpen, onDismissRequest = { menuOpen = false }) {
            DropdownMenuItem(onClick = { menuOpen = false; onSendSaveFile() }) {
                Text(stringResource(R.string.rom_action_send_save_file))
            }
            DropdownMenuItem(onClick = { menuOpen = false; onImportSaveFile() }) {
                Text(stringResource(R.string.rom_action_import_save_file))
            }
        }
    }
}

@Composable
fun NetplayButton(
    size: Dp,
    onClick: () -> Unit,
    modifier: Modifier = Modifier,
) {
    val interactionSource = remember { MutableInteractionSource() }
    val isFocused by interactionSource.collectIsFocusedAsState()
    val colors = watermelon
    Box(
        modifier = modifier
            .size(size)
            .clip(RoundedCornerShape(13.dp))
            .background(Color.Black.copy(alpha = 0.3f))
            .let { if (isFocused) it.background(colors.red.copy(alpha = 0.5f)) else it }
            .clickable(interactionSource = interactionSource, indication = null, onClick = onClick),
        contentAlignment = Alignment.Center,
    ) {
        Icon(Icons.Filled.Wifi, contentDescription = stringResource(R.string.netplay), tint = Color.White, modifier = Modifier.size(20.dp))
    }
}

@Composable
private fun HeroCover(
    rom: Rom,
    boxArtUrl: String?,
    raCoverUrl: String?,
    width: Dp,
    initialsSize: androidx.compose.ui.unit.TextUnit,
) {
    Box(
        modifier = Modifier
            .width(width)
            .aspectRatio(DsBoxArtAspectRatio)
            .shadow(10.dp, RoundedCornerShape(12.dp))
            .clip(RoundedCornerShape(12.dp))
            .background(romGradient(romDisplayName(rom))),
    ) {
        if (rom.config.iconSource != RomIconSource.DEFAULT) {
            WatermelonRomArt(
                rom = rom,
                boxArtUrl = null,
                raCoverUrl = raCoverUrl,
                initialsFontSize = initialsSize,
                modifier = Modifier.fillMaxSize(),
            )
        } else if (boxArtUrl != null) {
            AsyncImage(
                model = boxArtUrl,
                contentDescription = null,
                contentScale = ContentScale.Crop,
                modifier = Modifier.fillMaxSize(),
            )
        } else {
            Text(
                text = romInitials(romDisplayName(rom)),
                color = Color.White.copy(alpha = 0.22f),
                fontFamily = SpaceGrotesk,
                fontWeight = FontWeight.Bold,
                fontSize = initialsSize,
                modifier = Modifier.align(Alignment.Center),
            )
        }
        ScanlinesOverlay()
        RomMiniIcon(
            rom = rom,
            raCoverUrl = raCoverUrl,
            size = 26.dp,
            modifier = Modifier.align(Alignment.BottomStart).padding(5.dp),
        )
    }
}

@Composable
private fun HeroBackdrop(rom: Rom, boxArtUrl: String?, modifier: Modifier = Modifier) {
    val colors = watermelon
    Box(modifier = modifier) {
        if (boxArtUrl != null) {
            AsyncImage(
                model = boxArtUrl,
                contentDescription = null,
                contentScale = ContentScale.Crop,
                alpha = 0.75f,
                modifier = Modifier.fillMaxSize(),
            )
        } else {
            Box(Modifier.fillMaxSize().background(romGradient(romDisplayName(rom))))
        }
        Box(
            Modifier
                .fillMaxSize()
                .background(
                    Brush.verticalGradient(
                        colors = listOf(Color.Black.copy(alpha = 0.55f), colors.bg),
                    ),
                ),
        )
    }
}

@Composable
fun RomHeroVertical(
    rom: Rom,
    boxArtUrl: String?,
    raCoverUrl: String?,
    initialFocusRequester: FocusRequester,
    onLaunchRom: () -> Unit,
    onNavigateBack: () -> Unit,
    onSendSaveFile: () -> Unit,
    onImportSaveFile: () -> Unit,
    onNetplay: () -> Unit,
    modifier: Modifier = Modifier,
) {
    Box(modifier = modifier.fillMaxWidth()) {
        HeroBackdrop(rom = rom, boxArtUrl = boxArtUrl, modifier = Modifier.matchParentSize())
        Column(Modifier.padding(16.dp)) {
            Row(Modifier.fillMaxWidth()) {
                HeroCircleButton(Icons.AutoMirrored.Filled.ArrowBack, null, onNavigateBack)
                Spacer(Modifier.weight(1f))
            }
            Spacer(Modifier.height(14.dp))
            Row(verticalAlignment = Alignment.Bottom) {
                HeroCover(rom, boxArtUrl, raCoverUrl, width = 108.dp, initialsSize = 30.sp)
                Spacer(Modifier.width(12.dp))
                Column(Modifier.weight(1f)) {
                    Text(
                        text = romDisplayName(rom),
                        color = Color.White,
                        fontFamily = SpaceGrotesk,
                        fontSize = 21.sp,
                        lineHeight = 24.sp,
                        fontWeight = FontWeight.Bold,
                        maxLines = 3,
                        overflow = TextOverflow.Ellipsis,
                    )
                    if (rom.developerName.isNotBlank()) {
                        Text(
                            text = rom.developerName,
                            color = Color.White.copy(alpha = 0.75f),
                            fontSize = 12.sp,
                            maxLines = 1,
                            overflow = TextOverflow.Ellipsis,
                            modifier = Modifier.padding(top = 4.dp),
                        )
                    }
                    Row(verticalAlignment = Alignment.CenterVertically, modifier = Modifier.padding(top = 8.dp)) {
                        Box(
                            Modifier
                                .clip(RoundedCornerShape(5.dp))
                                .background(Color.Black.copy(alpha = 0.3f))
                                .padding(horizontal = 8.dp, vertical = 3.dp),
                        ) {
                            Text(
                                text = romPlatformLabel(rom),
                                color = Color.White,
                                fontFamily = WatermelonMono,
                                fontSize = 9.sp,
                                lineHeight = 9.sp,
                                fontWeight = FontWeight.SemiBold,
                                letterSpacing = 0.5.sp,
                            )
                        }
                        if (rom.totalPlayTime != Duration.ZERO) {
                            Text(
                                text = stringResource(R.string.rom_total_play_time_format, formatHoursLabel(rom.totalPlayTime)),
                                color = Color.White.copy(alpha = 0.75f),
                                fontFamily = WatermelonMono,
                                fontSize = 10.5.sp,
                                modifier = Modifier.padding(start = 8.dp),
                            )
                        }
                    }
                }
            }
            Spacer(Modifier.height(14.dp))
            Row {
                PlayButton(
                    height = 50.dp,
                    onClick = onLaunchRom,
                    focusRequester = initialFocusRequester,
                    modifier = Modifier.weight(1f),
                )
                Spacer(Modifier.width(9.dp))
                SaveActionsButton(size = 50.dp, onSendSaveFile = onSendSaveFile, onImportSaveFile = onImportSaveFile)
                Spacer(Modifier.width(9.dp))
                NetplayButton(size = 50.dp, onClick = onNetplay)
            }
        }
    }
}

@Composable
fun RomHeroSidePanel(
    rom: Rom,
    boxArtUrl: String?,
    raCoverUrl: String?,
    initialFocusRequester: FocusRequester,
    onLaunchRom: () -> Unit,
    onNavigateBack: () -> Unit,
    onSendSaveFile: () -> Unit,
    onImportSaveFile: () -> Unit,
    onNetplay: () -> Unit,
    modifier: Modifier = Modifier,
) {
    Box(modifier = modifier.width(252.dp).fillMaxHeight()) {
        HeroBackdrop(rom = rom, boxArtUrl = boxArtUrl, modifier = Modifier.matchParentSize())
        Column(
            Modifier
                .fillMaxSize()
                .padding(16.dp),
            verticalArrangement = Arrangement.SpaceBetween,
        ) {
            HeroCircleButton(Icons.AutoMirrored.Filled.ArrowBack, null, onNavigateBack)
            Column {
                Row(verticalAlignment = Alignment.Bottom) {
                    HeroCover(rom, boxArtUrl, raCoverUrl, width = 76.dp, initialsSize = 20.sp)
                    Spacer(Modifier.width(10.dp))
                    Box(
                        Modifier
                            .clip(RoundedCornerShape(5.dp))
                            .background(Color.Black.copy(alpha = 0.3f))
                            .padding(horizontal = 8.dp, vertical = 3.dp),
                    ) {
                        Text(
                            text = romPlatformLabel(rom),
                            color = Color.White,
                            fontFamily = WatermelonMono,
                            fontSize = 9.sp,
                            lineHeight = 9.sp,
                            fontWeight = FontWeight.SemiBold,
                        )
                    }
                }
                Text(
                    text = romDisplayName(rom),
                    color = Color.White,
                    fontFamily = SpaceGrotesk,
                    fontSize = 17.sp,
                    lineHeight = 20.sp,
                    fontWeight = FontWeight.Bold,
                    maxLines = 3,
                    overflow = TextOverflow.Ellipsis,
                    modifier = Modifier.padding(top = 10.dp),
                )
                if (rom.developerName.isNotBlank()) {
                    Text(
                        text = rom.developerName,
                        color = Color.White.copy(alpha = 0.75f),
                        fontSize = 12.sp,
                        maxLines = 1,
                        overflow = TextOverflow.Ellipsis,
                        modifier = Modifier.padding(top = 3.dp),
                    )
                }
                if (rom.totalPlayTime != Duration.ZERO) {
                    Text(
                        text = stringResource(R.string.rom_total_play_time_format, formatHoursLabel(rom.totalPlayTime)),
                        color = Color.White.copy(alpha = 0.75f),
                        fontFamily = WatermelonMono,
                        fontSize = 10.5.sp,
                        modifier = Modifier.padding(top = 6.dp),
                    )
                }
            }
            Row {
                PlayButton(
                    height = 42.dp,
                    onClick = onLaunchRom,
                    focusRequester = initialFocusRequester,
                    modifier = Modifier.weight(1f),
                )
                Spacer(Modifier.width(9.dp))
                SaveActionsButton(size = 42.dp, onSendSaveFile = onSendSaveFile, onImportSaveFile = onImportSaveFile)
                Spacer(Modifier.width(9.dp))
                NetplayButton(size = 42.dp, onClick = onNetplay)
            }
        }
    }
}

@Composable
fun RomDetailsTabRow(
    currentTab: RomDetailsTab,
    onTabClicked: (RomDetailsTab) -> Unit,
    modifier: Modifier = Modifier,
) {
    val colors = watermelon
    Column(modifier = modifier.fillMaxWidth().background(colors.bg)) {
        Row(
            horizontalArrangement = Arrangement.spacedBy(20.dp),
            modifier = Modifier.padding(horizontal = 20.dp),
        ) {
            RomDetailsTab.entries.forEach { tab ->
                val label = when (tab) {
                    RomDetailsTab.CONFIG -> stringResource(R.string.rom_details_configuration_tab)
                    RomDetailsTab.RETRO_ACHIEVEMENTS -> stringResource(R.string.retro_achievements_tab)
                    RomDetailsTab.OFFLINE_ACHIEVEMENTS -> stringResource(R.string.rom_details_offline_achievements_tab)
                }
                val selected = tab == currentTab
                val interactionSource = remember { MutableInteractionSource() }
                val isFocused by interactionSource.collectIsFocusedAsState()
                Column(
                    modifier = Modifier
                        .width(IntrinsicSize.Max)
                        .clickable(interactionSource = interactionSource, indication = null) { onTabClicked(tab) },
                ) {
                    Text(
                        text = label.uppercase(),
                        color = when {
                            isFocused -> colors.red
                            selected -> colors.text
                            else -> colors.text3
                        },
                        fontFamily = WatermelonMono,
                        fontSize = 10.sp,
                        fontWeight = FontWeight.SemiBold,
                        letterSpacing = 0.7.sp,
                        maxLines = 1,
                        softWrap = false,
                        modifier = Modifier.padding(top = 12.dp, bottom = 10.dp),
                    )
                    Box(
                        Modifier
                            .fillMaxWidth()
                            .height(2.dp)
                            .background(if (selected) colors.green else Color.Transparent),
                    )
                }
            }
        }
        Box(Modifier.fillMaxWidth().height(1.dp).background(colors.line))
    }
}

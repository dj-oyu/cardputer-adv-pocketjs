param([string]$Folder='.cache/dynamics-study')
$ErrorActionPreference='Stop'
Add-Type -AssemblyName System.Windows.Forms.DataVisualization
Add-Type -AssemblyName System.Drawing
$chart=[System.Windows.Forms.DataVisualization.Charting.Chart]::new()
$chart.Size=[System.Drawing.Size]::new(1280,920)
$chart.BackColor=[System.Drawing.Color]::White
$title=$chart.Titles.Add('HOST SIMULATION / binary64 / 60 Hz fixed step / not ESP32 timing')
$title.Font=[System.Drawing.Font]::new('Arial',15)
$title.Position=[System.Windows.Forms.DataVisualization.Charting.ElementPosition]::new(10,0,80,4)
$panels=@(
  @{title='Absolute angles: rigid vs soft (link .9 Hz, damping .35)';unit='Angle (deg)';items=@(
    @('rigid','q1','rigid',57.295779513),@('soft','q0','soft root',57.295779513),@('soft','q1','soft distal',57.295779513))},
  @{title='Geometric distance effect vs additional dynamic deflection';unit='Position (model unit)';items=@(
    @('soft','geom_x','rigid geometry at same root',1),@('soft','dynamic_x','extra from relative joint',1),@('soft','raw_x','actual raw tip X',1))},
  @{title='Error to pursuit reference; base offset is visual, not physical feedback';unit='Distance (model unit)';items=@(
    @('soft','residual','no compensation',1),@('held','residual','view hold, cap 16',1),@('limited','residual','view hold, cap .2',1))},
  @{title='Mutual load response: link 1.8 Hz / damping .7, same root drive';unit='Root angle (deg)';items=@(
    @('reference','q0','distal inertia 1',57.295779513),@('heavy','q0','distal inertia 2',57.295779513))}
)
$colors=@([System.Drawing.Color]::FromArgb(40,90,170),[System.Drawing.Color]::FromArgb(215,90,35),[System.Drawing.Color]::FromArgb(20,145,105))
$cache=@{}
for($index=0;$index -lt $panels.Count;$index++){
  $panel=$panels[$index]
  $area=[System.Windows.Forms.DataVisualization.Charting.ChartArea]::new('area'+$index)
  $area.Position=[System.Windows.Forms.DataVisualization.Charting.ElementPosition]::new(2+($index%2)*50,10+[Math]::Floor($index/2)*46,46,40)
  $area.AxisX.Minimum=0;$area.AxisX.Maximum=16;$area.AxisX.Interval=2
  $area.AxisX.Title='Simulation time (s); wall pause excluded'
  $area.AxisY.Title=$panel.unit
  $area.AxisX.MajorGrid.LineColor=[System.Drawing.Color]::Gainsboro
  $area.AxisY.MajorGrid.LineColor=[System.Drawing.Color]::Gainsboro
  $chart.ChartAreas.Add($area)
  $heading=$chart.Titles.Add($panel.title)
  $heading.DockedToChartArea=$area.Name;$heading.IsDockedInsideChartArea=$false
  $heading.Font=[System.Drawing.Font]::new('Arial',10)
  $legend=[System.Windows.Forms.DataVisualization.Charting.Legend]::new('legend'+$index)
  $legend.DockedToChartArea=$area.Name;$legend.Docking='Top';$legend.IsDockedInsideChartArea=$false
  $legend.Font=[System.Drawing.Font]::new('Arial',9)
  $chart.Legends.Add($legend)
  $j=0
  foreach($item in $panel.items){
    $name,$field,$label,$factor=$item
    if(-not $cache.ContainsKey($name)){$cache[$name]=Import-Csv (Join-Path $Folder ($name+'.csv'))}
    $series=[System.Windows.Forms.DataVisualization.Charting.Series]::new($index.ToString()+'-'+$label)
    $series.ChartArea=$area.Name;$series.Legend=$legend.Name;$series.LegendText=$label
    $series.ChartType='Line';$series.BorderWidth=2;$series.Color=$colors[$j%$colors.Count]
    foreach($row in $cache[$name]){[void]$series.Points.AddXY([double]$row.time,([double]$row.$field)*[double]$factor)}
    $chart.Series.Add($series);$j++
  }
  foreach($eventTime in @(2,4,6,8)){
    $strip=[System.Windows.Forms.DataVisualization.Charting.StripLine]::new()
    $strip.IntervalOffset=$eventTime;$strip.BorderColor=[System.Drawing.Color]::Gray;$strip.BorderDashStyle='Dash'
    $area.AxisX.StripLines.Add($strip)
  }
}
$chart.SaveImage((Join-Path (Resolve-Path $Folder) 'graphs.png'),'Png')
$chart.Dispose()
$chart=[System.Windows.Forms.DataVisualization.Charting.Chart]::new()
$chart.Size=[System.Drawing.Size]::new(1000,380)
$chart.BackColor=[System.Drawing.Color]::White
[void]$chart.Titles.Add('HOST: asymmetric branches / shared drive / schedule order equality, not multicore speed')
$subtitle=$chart.Titles.Add('Left: I1=1, .9/.35 -> 1.1/.9   Right: I1=1.6, 1.3/.6 -> .8/.45; different goals and event times')
$subtitle.Font=[System.Drawing.Font]::new('Arial',9)
$area=[System.Windows.Forms.DataVisualization.Charting.ChartArea]::new('main')
$area.AxisX.Title='Simulation time (s)';$area.AxisY.Title='Distal absolute angle (deg)'
$area.AxisX.Minimum=0;$area.AxisX.Maximum=16;$area.AxisX.Interval=2;$area.AxisX.LabelStyle.Format='0'
$area.AxisX.MajorGrid.LineColor=[System.Drawing.Color]::Gainsboro
$area.AxisY.MajorGrid.LineColor=[System.Drawing.Color]::Gainsboro
$chart.ChartAreas.Add($area)
$legend=[System.Windows.Forms.DataVisualization.Charting.Legend]::new('legend')
$legend.Docking='Bottom';$chart.Legends.Add($legend)
foreach($side in @('left','right')){
  $series=[System.Windows.Forms.DataVisualization.Charting.Series]::new($side)
  $series.ChartType='Line';$series.BorderWidth=2
  foreach($row in (Import-Csv (Join-Path $Folder ($side+'.csv')))){
    [void]$series.Points.AddXY([double]$row.time,([double]$row.q1)*57.295779513)
  }
  $chart.Series.Add($series)
}
$chart.SaveImage((Join-Path (Resolve-Path $Folder) 'asymmetric.png'),'Png')
$chart.Dispose()
Write-Output 'HOST_CHARTS_WRITTEN'
